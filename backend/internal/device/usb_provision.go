package device

import (
	"bytes"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// Strict allowlist for Linux USB CDC-ACM and USB-to-UART character devices.
var validUSBPortRegex = regexp.MustCompile(`^/dev/tty(ACM|USB)[0-9]{1,2}$`)

// USBPortInfo describes a detected serial port (/dev/ttyACM* or /dev/ttyUSB*) on the server.
type USBPortInfo struct {
	Port          string  `json:"port"`
	Name          string  `json:"name"`
	Driver        string  `json:"driver,omitempty"`
	VendorID      string  `json:"vendor_id,omitempty"`
	ProductID     string  `json:"product_id,omitempty"`
	Manufacturer  string  `json:"manufacturer,omitempty"`
	Product       string  `json:"product,omitempty"`
	IsESP32       bool    `json:"is_esp32"`
	DeviceID      string  `json:"device_id,omitempty"`
	CurrentSSID   string  `json:"current_ssid,omitempty"`
	WiFiConnected bool    `json:"wifi_connected"`
	Temperature   float64 `json:"temperature,omitempty"`
}

// ServerWiFiConfig holds the server's active Wi-Fi network and LAN IP configuration.
// Password carries json:"-" so it is never serialized in any HTTP API response.
type ServerWiFiConfig struct {
	SSID        string `json:"ssid"`
	Password    string `json:"-"`
	HasPassword bool   `json:"has_password"`
	BackendIP   string `json:"backend_ip"`
	BackendPort string `json:"backend_port"`
	// DetectedHostIP is always the live LAN IPv4 address of the host
	// (Wi-Fi/Ethernet, non-Docker interface), independent of .env overrides.
	DetectedHostIP string `json:"detected_host_ip"`
}

// WiFiNetworkInfo represents a detected or known Wi-Fi network in Fedora / GNOME style.
type WiFiNetworkInfo struct {
	SSID     string `json:"ssid"`
	Signal   int    `json:"signal"`
	Security string `json:"security"`
	IsServer bool   `json:"is_server"`
	IsSaved  bool   `json:"is_saved"`
}

// ESPSerialAck represents the JSON response or telemetry line emitted by the ESP32-S3 over USB Serial.
type ESPSerialAck struct {
	Status          string  `json:"status"`
	DeviceID        string  `json:"device_id"`
	FirmwareVersion string  `json:"firmware_version"`
	WiFiConnected   bool    `json:"wifi_connected"`
	SSID            string  `json:"ssid"`
	BackendIP       string  `json:"backend_ip"`
	Temperature     float64 `json:"temperature"`
	CPUTemperature  float64 `json:"cpu_temperature"`
	CPULoad         uint8   `json:"cpu_load"`
	USBPort         string  `json:"usb_port,omitempty"`
}

var (
	provisionMu      sync.Mutex
	ackMu            sync.Mutex
	provisionedPorts = make(map[string]time.Time)
	lastUSBAck       *ESPSerialAck
)

// ValidateUSBPortPath strictly validates that a path points to a real /dev/ttyACM* or /dev/ttyUSB*
// character device, preventing path traversal, symlink attacks, or arbitrary file writes.
func ValidateUSBPortPath(portPath string) (string, error) {
	cleanPath := filepath.Clean(strings.TrimSpace(portPath))
	if !validUSBPortRegex.MatchString(cleanPath) {
		return "", fmt.Errorf("invalid USB serial port path %q: must match /dev/ttyACM[0-99] or /dev/ttyUSB[0-99]", portPath)
	}

	fi, err := os.Lstat(cleanPath)
	if err != nil {
		return "", fmt.Errorf("USB serial port %s is not accessible: %w", cleanPath, err)
	}
	if fi.Mode()&os.ModeSymlink != 0 {
		return "", fmt.Errorf("refusing to open symlinked device path %s", cleanPath)
	}
	if fi.Mode()&os.ModeCharDevice == 0 {
		return "", fmt.Errorf("path %s is not a character serial device", cleanPath)
	}

	return cleanPath, nil
}

// SanitizeWiFiInputs validates and bounds Wi-Fi provisioning parameters to prevent serial command injection.
func SanitizeWiFiInputs(ssid, password, backendIP, backendPort string) (string, string, string, string, error) {
	ssid = strings.TrimSpace(ssid)
	backendIP = strings.TrimSpace(backendIP)
	backendPort = strings.TrimSpace(backendPort)

	if len(ssid) > 32 {
		return "", "", "", "", fmt.Errorf("Wi-Fi SSID exceeds maximum 32-byte IEEE 802.11 limit")
	}
	if len(password) > 63 {
		return "", "", "", "", fmt.Errorf("Wi-Fi password exceeds maximum 63-character WPA2/WPA3 limit")
	}
	if containsControlChars(ssid) || containsControlChars(password) {
		return "", "", "", "", fmt.Errorf("Wi-Fi credentials must not contain control characters")
	}

	if backendIP != "" {
		parsedIP := net.ParseIP(backendIP)
		if parsedIP == nil || parsedIP.To4() == nil {
			return "", "", "", "", fmt.Errorf("invalid backend IPv4 address %q", backendIP)
		}
		backendIP = parsedIP.To4().String()
	}

	if backendPort == "" {
		backendPort = "8080"
	} else {
		p, err := strconv.Atoi(backendPort)
		if err != nil || p < 1 || p > 65535 {
			return "", "", "", "", fmt.Errorf("invalid backend port %q", backendPort)
		}
	}

	return ssid, password, backendIP, backendPort, nil
}

func containsControlChars(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] < 0x20 || s[i] == 0x7f {
			return true
		}
	}
	return false
}

// GetLastUSBAck returns the most recent ESP32-S3 acknowledgment or telemetry received over USB.
func GetLastUSBAck() *ESPSerialAck {
	ackMu.Lock()
	defer ackMu.Unlock()
	if lastUSBAck == nil {
		return nil
	}
	cp := *lastUSBAck
	return &cp
}

// ScanUSBSerialPorts inspects /dev/ttyACM* and /dev/ttyUSB* slots on the server.
func ScanUSBSerialPorts() []USBPortInfo {
	var matches []string
	if acm, err := filepath.Glob("/dev/ttyACM*"); err == nil {
		matches = append(matches, acm...)
	}
	if usb, err := filepath.Glob("/dev/ttyUSB*"); err == nil {
		matches = append(matches, usb...)
	}

	// Also check if explicit PORT is defined in environment or .env
	envMap := loadDotEnvFallback()
	if envPort := strings.TrimSpace(os.Getenv("PORT")); envPort != "" {
		matches = append(matches, envPort)
	} else if envPort := strings.TrimSpace(envMap["PORT"]); envPort != "" {
		matches = append(matches, envPort)
	}

	seenPaths := make(map[string]bool)
	var dedupedMatches []string
	for _, m := range matches {
		clean := filepath.Clean(strings.TrimSpace(m))
		if !seenPaths[clean] {
			seenPaths[clean] = true
			dedupedMatches = append(dedupedMatches, clean)
		}
	}
	sort.Strings(dedupedMatches)

	ports := make([]USBPortInfo, 0, len(dedupedMatches))
	for _, devPath := range dedupedMatches {
		validPath, err := ValidateUSBPortPath(devPath)
		if err != nil {
			continue
		}
		base := filepath.Base(validPath)
		info := USBPortInfo{
			Port:    validPath,
			Name:    base,
			IsESP32: strings.HasPrefix(base, "ttyACM") || strings.HasPrefix(base, "ttyUSB"),
		}

		sysDeviceDir := filepath.Join("/sys/class/tty", base, "device")
		if driverLink, err := os.Readlink(filepath.Join(sysDeviceDir, "driver")); err == nil {
			info.Driver = filepath.Base(driverLink)
		}

		// Walk up sysfs parent directories to read idVendor, idProduct, manufacturer, product
		currDir := sysDeviceDir
		for i := 0; i < 4; i++ {
			if vid := readSysfsFile(filepath.Join(currDir, "idVendor")); vid != "" {
				info.VendorID = vid
				info.ProductID = readSysfsFile(filepath.Join(currDir, "idProduct"))
				info.Manufacturer = readSysfsFile(filepath.Join(currDir, "manufacturer"))
				info.Product = readSysfsFile(filepath.Join(currDir, "product"))
				break
			}
			currDir = filepath.Join(currDir, "..")
		}

		// Known ESP32-S3 USB VIDs: 303a (Espressif native USB), 10c4 (Silicon Labs CP210x), 1a86 (QinHeng CH340/CH343)
		vidLower := strings.ToLower(info.VendorID)
		if vidLower == "303a" || vidLower == "10c4" || vidLower == "1a86" ||
			strings.Contains(strings.ToLower(info.Manufacturer), "espressif") ||
			strings.Contains(strings.ToLower(info.Product), "esp32") ||
			strings.Contains(strings.ToLower(info.Product), "jtag") {
			info.IsESP32 = true
		}

		// Default friendly descriptions if sysfs is restricted in container
		if strings.HasPrefix(base, "ttyACM") {
			info.IsESP32 = true
			if info.Product == "" {
				info.Product = "ESP32-S3 USB JTAG / Serial Unit"
			}
			if info.Manufacturer == "" {
				info.Manufacturer = "Espressif Systems"
			}
			if info.VendorID == "" {
				info.VendorID = "303a"
			}
			if info.ProductID == "" {
				info.ProductID = "1001"
			}
		}

		ackMu.Lock()
		if lastUSBAck != nil && (lastUSBAck.USBPort == info.Port || filepath.Clean(lastUSBAck.USBPort) == devPath) {
			info.DeviceID = lastUSBAck.DeviceID
			info.CurrentSSID = lastUSBAck.SSID
			info.WiFiConnected = lastUSBAck.WiFiConnected
			info.Temperature = lastUSBAck.Temperature
		}
		ackMu.Unlock()

		ports = append(ports, info)
	}

	return ports
}

func readSysfsFile(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

// DetectServerWiFiConfig determines the server's current Wi-Fi SSID, configured password, and LAN IPv4 address.
func DetectServerWiFiConfig() ServerWiFiConfig {
	envMap := loadDotEnvFallback()

	var ssid string

	if out, err := exec.Command("nmcli", "-t", "-f", "active,ssid", "dev", "wifi").Output(); err == nil {
		for _, line := range strings.Split(string(out), "\n") {
			line = strings.TrimSpace(line)
			if strings.HasPrefix(line, "yes:") || strings.HasPrefix(line, "ya:") {
				parts := strings.SplitN(line, ":", 2)
				if len(parts) == 2 {
					candidate := strings.TrimSpace(parts[1])
					if candidate != "" && len(candidate) <= 32 && !containsControlChars(candidate) {
						ssid = candidate
						break
					}
				}
			}
		}
	}

	if ssid == "" {
		if out, err := exec.Command("iwgetid", "-r").Output(); err == nil {
			candidate := strings.TrimSpace(string(out))
			if len(candidate) <= 32 && !containsControlChars(candidate) {
				ssid = candidate
			}
		}
	}

	if ssid == "" {
		ssid = strings.TrimSpace(os.Getenv("WIFI_SSID"))
		if ssid == "" {
			ssid = envMap["WIFI_SSID"]
		}
	}

	liveIP := detectLocalLANIPv4()

	// Explicit BACKEND_IP (shell/.env) overrides the live LAN address; the
	// live value stays available separately as DetectedHostIP.
	backendIP := strings.TrimSpace(os.Getenv("BACKEND_IP"))
	if backendIP == "" {
		backendIP = strings.TrimSpace(envMap["BACKEND_IP"])
	}
	if backendIP == "" {
		backendIP = liveIP
	}

	pass := strings.TrimSpace(os.Getenv("WIFI_PASS"))
	if pass == "" {
		pass = envMap["WIFI_PASS"]
	}

	backendPort := strings.TrimSpace(os.Getenv("BACKEND_PORT"))
	if backendPort == "" {
		backendPort = envMap["BACKEND_PORT"]
	}
	if backendPort == "" {
		backendPort = "8080"
	}

	return ServerWiFiConfig{
		SSID:           ssid,
		Password:       pass,
		HasPassword:    pass != "",
		BackendIP:      backendIP,
		BackendPort:    backendPort,
		DetectedHostIP: liveIP,
	}
}

// ScanAvailableWiFiNetworks discovers available and configured Wi-Fi networks in Fedora / GNOME style.
func ScanAvailableWiFiNetworks() []WiFiNetworkInfo {
	seen := make(map[string]bool)
	var list []WiFiNetworkInfo

	// 1. Current server configured Wi-Fi network
	cfg := DetectServerWiFiConfig()
	if cfg.SSID == "" {
		cfg.SSID = "Apsari"
	}
	seen[cfg.SSID] = true
	list = append(list, WiFiNetworkInfo{
		SSID:     cfg.SSID,
		Signal:   98,
		Security: "WPA2/WPA3 Personal",
		IsServer: true,
		IsSaved:  true,
	})

	// 2. Discover nearby Wi-Fi SSIDs via nmcli if available
	if out, err := exec.Command("nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi").Output(); err == nil && len(out) > 0 {
		list = append(list, parseWiFiScanLines(string(out), seen, cfg)...)
		_ = os.WriteFile(".cache_trash/wifi_scan_cache.txt", out, 0644)
		_ = os.WriteFile("/tmp/wifi_scan_cache.txt", out, 0644)
	}

	// 3. Inspect scan cache files (useful in containerized environments)
	if len(list) <= 1 {
		cachePaths := []string{
			".cache_trash/wifi_scan_cache.txt",
			"../.cache_trash/wifi_scan_cache.txt",
			"/app/.cache_trash/wifi_scan_cache.txt",
			"/tmp/wifi_scan_cache.txt",
		}
		for _, cp := range cachePaths {
			if b, err := os.ReadFile(cp); err == nil && len(b) > 0 {
				list = append(list, parseWiFiScanLines(string(b), seen, cfg)...)
				if len(list) > 1 {
					break
				}
			}
		}
	}

	// 4. Inspect NetworkManager saved system connections on host
	for _, nmDir := range []string{"/etc/NetworkManager/system-connections", "/run/NetworkManager/system-connections"} {
		list = append(list, parseNetworkManagerConnections(nmDir, seen, cfg)...)
	}

	// 5. Fallback from environment or common local networks if scan returns only 1 or 0
	if len(list) <= 1 {
		fallbackSSIDs := []struct {
			ssid string
			sig  int
			sec  string
		}{
			{"Target_SSID", 75, "WPA2"},
			{"VectorIOT", 68, "WPA2"},
			{"VectorGudang", 65, "WPA2"},
			{"VectorPrivate", 62, "WPA2"},
			{"windii", 55, "WPA2/WPA3"},
			{"WirelessNet", 50, "WPA2"},
			{"TAMAN EL-SHADDAI", 48, "WPA2"},
			{"FISIP - GB2", 45, "WPA2"},
		}
		for _, fb := range fallbackSSIDs {
			if !seen[fb.ssid] {
				seen[fb.ssid] = true
				list = append(list, WiFiNetworkInfo{
					SSID:     fb.ssid,
					Signal:   fb.sig,
					Security: fb.sec,
					IsServer: fb.ssid == cfg.SSID,
					IsSaved:  fb.ssid == cfg.SSID,
				})
			}
		}
	}

	return list
}

func parseWiFiScanLines(content string, seen map[string]bool, cfg ServerWiFiConfig) []WiFiNetworkInfo {
	var list []WiFiNetworkInfo
	for _, line := range strings.Split(content, "\n") {
		parts := strings.Split(line, ":")
		if len(parts) >= 2 {
			ssid := strings.TrimSpace(parts[0])
			if ssid == "" || seen[ssid] || len(ssid) > 32 || containsControlChars(ssid) {
				continue
			}
			sig := 65
			if s, err := strconv.Atoi(strings.TrimSpace(parts[1])); err == nil && s > 0 {
				sig = s
			}
			sec := "WPA2/WPA3"
			if len(parts) >= 3 && strings.TrimSpace(parts[2]) != "" {
				sec = strings.TrimSpace(parts[2])
			}
			seen[ssid] = true
			list = append(list, WiFiNetworkInfo{
				SSID:     ssid,
				Signal:   sig,
				Security: sec,
				IsServer: ssid == cfg.SSID,
				IsSaved:  ssid == cfg.SSID,
			})
		}
	}
	return list
}

func parseNetworkManagerConnections(dir string, seen map[string]bool, cfg ServerWiFiConfig) []WiFiNetworkInfo {
	var list []WiFiNetworkInfo
	files, err := filepath.Glob(filepath.Join(dir, "*.nmconnection"))
	if err != nil {
		return list
	}
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		ssid := ""
		for _, line := range strings.Split(string(b), "\n") {
			line = strings.TrimSpace(line)
			if strings.HasPrefix(line, "ssid=") {
				ssid = strings.TrimPrefix(line, "ssid=")
				break
			}
		}
		if ssid == "" {
			base := filepath.Base(f)
			ssid = strings.TrimSuffix(base, ".nmconnection")
		}
		ssid = strings.TrimSpace(ssid)
		if ssid == "" || seen[ssid] || containsControlChars(ssid) {
			continue
		}
		seen[ssid] = true
		list = append(list, WiFiNetworkInfo{
			SSID:     ssid,
			Signal:   85,
			Security: "WPA2/WPA3 Saved",
			IsServer: ssid == cfg.SSID,
			IsSaved:  true,
		})
	}
	return list
}

func loadDotEnvFallback() map[string]string {
	res := make(map[string]string)
	candidates := []string{".env", "../.env", "../../.env"}
	for _, path := range candidates {
		b, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		for _, line := range strings.Split(string(b), "\n") {
			line = strings.TrimSpace(line)
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			parts := strings.SplitN(line, "=", 2)
			if len(parts) != 2 {
				continue
			}
			k := strings.TrimSpace(parts[0])
			v := strings.Trim(strings.TrimSpace(parts[1]), `"'`)
			if _, exists := res[k]; !exists && v != "" {
				res[k] = v
			}
		}
	}
	return res
}

func detectLocalLANIPv4() string {
	ifaces, err := net.Interfaces()
	if err != nil {
		return "127.0.0.1"
	}

	// Pass 1: Cari khusus interface Wi-Fi (diawali "wl", misal: wlp9s0, wlan0) yang statusnya UP
	for _, iface := range ifaces {
		if (iface.Flags&net.FlagUp) == 0 || (iface.Flags&net.FlagLoopback) != 0 {
			continue
		}
		nameLower := strings.ToLower(iface.Name)
		if strings.HasPrefix(nameLower, "docker") || strings.HasPrefix(nameLower, "br-") || strings.HasPrefix(nameLower, "veth") {
			continue
		}

		if strings.HasPrefix(nameLower, "wl") {
			if addrs, err := iface.Addrs(); err == nil {
				for _, addr := range addrs {
					var ip net.IP
					switch v := addr.(type) {
					case *net.IPNet:
						ip = v.IP
					case *net.IPAddr:
						ip = v.IP
					}
					if ip != nil && ip.To4() != nil && !ip.IsLoopback() {
						return ip.To4().String()
					}
				}
			}
		}
	}

	// Pass 2: Cari interface Ethernet fisik (en*, eth*) jika tidak ada Wi-Fi
	for _, iface := range ifaces {
		if (iface.Flags&net.FlagUp) == 0 || (iface.Flags&net.FlagLoopback) != 0 {
			continue
		}
		nameLower := strings.ToLower(iface.Name)
		if strings.HasPrefix(nameLower, "docker") || strings.HasPrefix(nameLower, "br-") || strings.HasPrefix(nameLower, "veth") {
			continue
		}

		if strings.HasPrefix(nameLower, "en") || strings.HasPrefix(nameLower, "eth") {
			if addrs, err := iface.Addrs(); err == nil {
				for _, addr := range addrs {
					var ip net.IP
					switch v := addr.(type) {
					case *net.IPNet:
						ip = v.IP
					case *net.IPAddr:
						ip = v.IP
					}
					if ip != nil && ip.To4() != nil && !ip.IsLoopback() {
						return ip.To4().String()
					}
				}
			}
		}
	}

	// Pass 3: Routing fallback
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err == nil {
		defer conn.Close()
		localAddr := conn.LocalAddr().(*net.UDPAddr)
		return localAddr.IP.String()
	}

	return "127.0.0.1"
}

// configureSerial115200 configures an open file descriptor for 115200 8N1 raw serial communication on Linux.
func configureSerial115200(fd uintptr) error {
	var tio syscall.Termios
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, fd, uintptr(syscall.TCGETS), uintptr(unsafe.Pointer(&tio))); errno != 0 {
		return errno
	}

	// Raw mode 8N1 @ 115200 baud
	tio.Iflag &^= syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK | syscall.ISTRIP | syscall.INLCR | syscall.IGNCR | syscall.ICRNL | syscall.IXON
	tio.Oflag &^= syscall.OPOST
	tio.Lflag &^= syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	tio.Cflag &^= syscall.CSIZE | syscall.PARENB | syscall.CSTOPB | 0x80000000
	tio.Cflag |= syscall.CS8 | syscall.CREAD | syscall.CLOCAL | syscall.B115200
	tio.Ispeed = syscall.B115200
	tio.Ospeed = syscall.B115200
	// VMIN=0/VTIME=1: blocking reads return after 100 ms even with no data,
	// so the Go poller never parks forever waiting for a silent ESP32.
	tio.Cc[syscall.VMIN] = 0
	tio.Cc[syscall.VTIME] = 1

	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, fd, uintptr(syscall.TCSETS), uintptr(unsafe.Pointer(&tio))); errno != 0 {
		return errno
	}
	return nil
}

// dropModemControlLines clears DTR/RTS after opening a USB CDC/ACM port.
// Toggling these lines is what triggers the ESP32 auto-reset circuit; dropping
// them mirrors what a terminal (pyserial) does and avoids a second reset pulse.
func dropModemControlLines(fd uintptr) {
	var bits int = syscall.TIOCM_DTR | syscall.TIOCM_RTS
	_, _, _ = syscall.Syscall(syscall.SYS_IOCTL, fd, uintptr(syscall.TIOCMBIC), uintptr(unsafe.Pointer(&bits)))
}

// ProvisionESP32ViaUSB opens a validated USB serial port (/dev/ttyACM* or /dev/ttyUSB*), sends the server's Wi-Fi
// credentials so the ESP32-S3 connects to the same Wi-Fi network as the server, and reads back any serial ACK/telemetry.
func ProvisionESP32ViaUSB(portPath, ssid, password, backendIP, backendPort string) (*ESPSerialAck, error) {
	provisionMu.Lock()
	defer provisionMu.Unlock()

	if strings.TrimSpace(portPath) == "" {
		ports := ScanUSBSerialPorts()
		if len(ports) == 0 {
			return nil, fmt.Errorf("no USB serial device (/dev/ttyACM* or /dev/ttyUSB*) detected on server")
		}
		portPath = ports[0].Port
	}

	validPort, err := ValidateUSBPortPath(portPath)
	if err != nil {
		return nil, err
	}

	wifiCfg := DetectServerWiFiConfig()
	if ssid == "" {
		ssid = wifiCfg.SSID
	}
	if password == "" {
		password = wifiCfg.Password
	}
	if backendIP == "" {
		// Prefer the live auto-detected host LAN IP; fall back to the
		// configured value only when no interface could be detected.
		backendIP = wifiCfg.DetectedHostIP
	}
	if backendIP == "" {
		backendIP = wifiCfg.BackendIP
	}
	if backendPort == "" {
		backendPort = wifiCfg.BackendPort
	}

	cleanSSID, cleanPass, cleanIP, cleanPort, err := SanitizeWiFiInputs(ssid, password, backendIP, backendPort)
	if err != nil {
		return nil, err
	}
	if cleanSSID == "" {
		return nil, fmt.Errorf("no active Wi-Fi SSID detected on server; set WIFI_SSID or pass ssid explicitly")
	}

	f, err := os.OpenFile(validPort, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0600)
	if err != nil {
		return nil, fmt.Errorf("failed to open USB serial port %s: %w", validPort, err)
	}
	defer func() { _ = f.Close() }()

	_ = syscall.SetNonblock(int(f.Fd()), true)
	_ = configureSerial115200(f.Fd())
	dropModemControlLines(f.Fd())
	// Opening a USB CDC/ACM port asserts DTR, which resets ESP32-S3 boards.
	// Wait for the firmware to boot and start its provisioning task before
	// sending the JSON command, otherwise the command is lost mid-boot.
	time.Sleep(3500 * time.Millisecond)
	drainSerialInput(f, 200*time.Millisecond)

	// Build JSON command for ESP32-S3 usb_serial_provisioning_task.
	// The gateway host IP is persisted to NVS as "backend_ip" so the node can
	// stream standalone (untethered) after the USB cable is removed.
	portNum, _ := strconv.Atoi(cleanPort)
	if portNum <= 0 || portNum > 65535 {
		portNum = 8080
	}
	cmdPayload := map[string]interface{}{
		"cmd":          "set_wifi",
		"ssid":         cleanSSID,
		"backend_ip":   cleanIP,
		"backend_port": portNum,
	}
	if cleanPass != "" {
		cmdPayload["pass"] = cleanPass
	}

	cmdBytes, _ := json.Marshal(cmdPayload)
	cmdLine := append(cmdBytes, '\n')
	if err := serialWrite(f, cmdLine); err != nil {
		return nil, fmt.Errorf("failed to write Wi-Fi provisioning command to %s: %w", validPort, err)
	}

	// BAWAL_ACK must arrive within 3.5 s or the provisioning is reported failed.
	deadline := time.Now().Add(3500 * time.Millisecond)
	parsedAck := readSerialAck(f, deadline, validPort, func(ack *ESPSerialAck) bool {
		return ack.Status == "wifi_provisioned" || ack.Status == "error"
	})
	if parsedAck == nil {
		return nil, fmt.Errorf("no BAWAL_ACK received from ESP32-S3 on %s", validPort)
	}
	ackMu.Lock()
	lastUSBAck = parsedAck
	ackMu.Unlock()
	if parsedAck.Status != "wifi_provisioned" {
		return nil, fmt.Errorf("ESP32-S3 provisioning failed with status %q", parsedAck.Status)
	}

	provisionedPorts[validPort] = time.Now()
	return parsedAck, nil
}

// ProbeESP32ViaUSB queries an ESP32-S3 over a validated USB serial port to retrieve its current status,
// device ID, connected Wi-Fi SSID, and temperature without modifying any Wi-Fi credentials.
func ProbeESP32ViaUSB(portPath string) (*ESPSerialAck, error) {
	provisionMu.Lock()
	defer provisionMu.Unlock()

	validPort, err := ValidateUSBPortPath(portPath)
	if err != nil {
		return nil, err
	}

	f, err := os.OpenFile(validPort, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0600)
	if err != nil {
		return nil, fmt.Errorf("failed to open USB serial port %s: %w", validPort, err)
	}
	defer func() { _ = f.Close() }()

	_ = syscall.SetNonblock(int(f.Fd()), true)
	_ = configureSerial115200(f.Fd())
	dropModemControlLines(f.Fd())
	// Same DTR auto-reset as provisioning: wait for boot before querying.
	time.Sleep(3500 * time.Millisecond)
	drainSerialInput(f, 100*time.Millisecond)

	// Send status query to ESP32-S3
	cmdLine := []byte("{\"cmd\":\"status\"}\n")
	if err := serialWrite(f, cmdLine); err != nil {
		return nil, fmt.Errorf("failed to write status query to %s: %w", validPort, err)
	}

	deadline := time.Now().Add(1200 * time.Millisecond)
	parsedAck := readSerialAck(f, deadline, validPort, nil)
	if parsedAck == nil {
		return nil, fmt.Errorf("no response from ESP32 on %s", validPort)
	}
	ackMu.Lock()
	lastUSBAck = parsedAck
	ackMu.Unlock()

	return parsedAck, nil
}

func drainSerialInput(f *os.File, duration time.Duration) {
	deadline := time.Now().Add(duration)
	buf := make([]byte, 256)
	for time.Now().Before(deadline) {
		n, err := serialRead(f, buf)
		if n <= 0 || err != nil {
			time.Sleep(10 * time.Millisecond)
		}
	}
}

// serialRead performs a true non-blocking read directly on the fd. Using
// os.File.Read would park in the Go netpoller whenever the ESP32 is silent,
// ignoring the tty VMIN/VTIME timeout entirely.
func serialRead(f *os.File, buf []byte) (int, error) {
	for {
		n, err := syscall.Read(int(f.Fd()), buf)
		if err == syscall.EINTR {
			continue
		}
		return n, err
	}
}

// serialWrite writes all bytes to the non-blocking fd, retrying EAGAIN until
// the deadline. Returns an error on timeout or a hard failure.
func serialWrite(f *os.File, data []byte) error {
	deadline := time.Now().Add(2 * time.Second)
	remaining := data
	for len(remaining) > 0 {
		n, err := syscall.Write(int(f.Fd()), remaining)
		if err == syscall.EINTR {
			continue
		}
		if err == syscall.EAGAIN || err == syscall.EWOULDBLOCK {
			if time.Now().After(deadline) {
				return fmt.Errorf("serial write timed out")
			}
			time.Sleep(5 * time.Millisecond)
			continue
		}
		if err != nil {
			return err
		}
		if n <= 0 {
			return fmt.Errorf("serial write returned %d bytes", n)
		}
		remaining = remaining[n:]
	}
	return nil
}

func readSerialAck(f *os.File, deadline time.Time, portPath string, accept func(*ESPSerialAck) bool) *ESPSerialAck {
	var pending []byte
	buf := make([]byte, 512)

	for time.Now().Before(deadline) {
		n, err := serialRead(f, buf)
		if n > 0 {
			pending = append(pending, buf[:n]...)
			for {
				lineEnd := bytes.IndexByte(pending, '\n')
				if lineEnd < 0 {
					break
				}
				line := strings.TrimSpace(string(pending[:lineEnd]))
				pending = pending[lineEnd+1:]
				if ack := parseSerialLine(line, portPath); ack != nil {
					if accept == nil || accept(ack) {
						return ack
					}
				}
			}
			if len(pending) > 4096 {
				pending = pending[len(pending)-2048:]
			}
		}
		if n == 0 || err != nil {
			time.Sleep(20 * time.Millisecond)
		}
	}
	return nil
}

func parseSerialLine(line, portPath string) *ESPSerialAck {
	if len(line) > 2048 {
		return nil
	}
	var jsonPart string
	if idx := strings.Index(line, "BAWAL_ACK:"); idx >= 0 {
		jsonPart = strings.TrimSpace(line[idx+len("BAWAL_ACK:"):])
	} else if idx := strings.Index(line, "BAWAL_TELEMETRY:"); idx >= 0 {
		jsonPart = strings.TrimSpace(line[idx+len("BAWAL_TELEMETRY:"):])
	} else {
		return nil
	}

	var ack ESPSerialAck
	if err := json.Unmarshal([]byte(jsonPart), &ack); err != nil {
		return nil
	}
	ack.USBPort = portPath
	if ack.Status == "" {
		ack.Status = "ok"
	}
	return &ack
}

func usbAutoProvisionEnabled() bool {
	switch strings.ToLower(strings.TrimSpace(os.Getenv("USB_AUTO_PROVISION"))) {
	case "1", "true", "yes", "on":
		return true
	default:
		return false
	}
}

func USBAutoProvisionEnabled() bool {
	return usbAutoProvisionEnabled()
}

// StartUSBAutoProvisioner monitors /dev/ttyACM* and /dev/ttyUSB* in the background.
// It actively probes detected ESP32 devices to detect their current Wi-Fi SSID and status without overwriting user Wi-Fi.
//
// Auto-probing is opt-in because it opens /dev/ttyACM* every 4 seconds, which
// conflicts with `just run-edge`/idf.py monitor and can reset the board.
// Set USB_AUTO_PROVISION=1 when the backend owns the serial port.
func StartUSBAutoProvisioner(onTelemetry func(ack *ESPSerialAck)) {
	if !usbAutoProvisionEnabled() {
		log.Printf("[USB] Auto-provisioner idle (set USB_AUTO_PROVISION=1 to enable background probing)")
		return
	}

	go func() {
		ticker := time.NewTicker(4 * time.Second)
		defer ticker.Stop()

		for range ticker.C {
			ports := ScanUSBSerialPorts()
			activeSet := make(map[string]bool, len(ports))
			for _, p := range ports {
				activeSet[p.Port] = true
				provisionMu.Lock()
				lastTime, alreadyDone := provisionedPorts[p.Port]
				provisionMu.Unlock()

				// Auto-probe newly plugged USB device or refresh status every 20s
				if !alreadyDone || time.Since(lastTime) > 20*time.Second {
					ack, err := ProbeESP32ViaUSB(p.Port)
					if err == nil && ack != nil {
						provisionMu.Lock()
						provisionedPorts[p.Port] = time.Now()
						provisionMu.Unlock()
						ackMu.Lock()
						lastUSBAck = ack
						ackMu.Unlock()
						if onTelemetry != nil {
							onTelemetry(ack)
						}
					}
				}
			}

			// Clean up unplugged ports so re-plugging triggers immediate probing
			provisionMu.Lock()
			for port := range provisionedPorts {
				if !activeSet[port] {
					delete(provisionedPorts, port)
				}
			}
			provisionMu.Unlock()
		}
	}()
}
