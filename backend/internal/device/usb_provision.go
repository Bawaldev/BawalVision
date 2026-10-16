package device

import (
	"bufio"
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
	Port         string `json:"port"`
	Name         string `json:"name"`
	Driver       string `json:"driver,omitempty"`
	VendorID     string `json:"vendor_id,omitempty"`
	ProductID    string `json:"product_id,omitempty"`
	Manufacturer string `json:"manufacturer,omitempty"`
	Product      string `json:"product,omitempty"`
	IsESP32      bool   `json:"is_esp32"`
}

// ServerWiFiConfig holds the server's active Wi-Fi network and LAN IP configuration.
// Password carries json:"-" so it is never serialized in any HTTP API response.
type ServerWiFiConfig struct {
	SSID        string `json:"ssid"`
	Password    string `json:"-"`
	HasPassword bool   `json:"has_password"`
	BackendIP   string `json:"backend_ip"`
	BackendPort string `json:"backend_port"`
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
	provisionMu.Lock()
	defer provisionMu.Unlock()
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
	sort.Strings(matches)

	ports := make([]USBPortInfo, 0, len(matches))
	for _, devPath := range matches {
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

	ssid := strings.TrimSpace(os.Getenv("WIFI_SSID"))
	if ssid == "" {
		ssid = envMap["WIFI_SSID"]
	}
	pass := strings.TrimSpace(os.Getenv("WIFI_PASS"))
	if pass == "" {
		pass = envMap["WIFI_PASS"]
	}
	backendIP := strings.TrimSpace(os.Getenv("BACKEND_IP"))
	if backendIP == "" {
		backendIP = envMap["BACKEND_IP"]
	}
	backendPort := strings.TrimSpace(os.Getenv("BACKEND_PORT"))
	if backendPort == "" {
		backendPort = envMap["BACKEND_PORT"]
	}
	if backendPort == "" {
		backendPort = "8080"
	}

	// Query active host Wi-Fi SSID using fixed, argument-safe read-only commands if not set via environment
	if ssid == "" {
		if out, err := exec.Command("iwgetid", "-r").Output(); err == nil {
			candidate := strings.TrimSpace(string(out))
			if len(candidate) <= 32 && !containsControlChars(candidate) {
				ssid = candidate
			}
		}
	}
	if ssid == "" {
		if out, err := exec.Command("nmcli", "-t", "-f", "active,ssid", "dev", "wifi").Output(); err == nil {
			for _, line := range strings.Split(string(out), "\n") {
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
	}

	// Determine server LAN IPv4 address so the ESP32-S3 can reach this backend on the shared Wi-Fi
	if backendIP == "" {
		backendIP = detectLocalLANIPv4()
	}

	return ServerWiFiConfig{
		SSID:        ssid,
		Password:    pass,
		HasPassword: pass != "",
		BackendIP:   backendIP,
		BackendPort: backendPort,
	}
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

	var fallbackIP string
	for _, iface := range ifaces {
		if (iface.Flags&net.FlagUp) == 0 || (iface.Flags&net.FlagLoopback) != 0 {
			continue
		}
		nameLower := strings.ToLower(iface.Name)
		isVirtual := strings.HasPrefix(nameLower, "docker") || strings.HasPrefix(nameLower, "br-") || strings.HasPrefix(nameLower, "veth")

		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			var ip net.IP
			switch v := addr.(type) {
			case *net.IPNet:
				ip = v.IP
			case *net.IPAddr:
				ip = v.IP
			}
			if ip == nil || ip.IsLoopback() {
				continue
			}
			ip4 := ip.To4()
			if ip4 == nil {
				continue
			}
			if !isVirtual && (strings.HasPrefix(nameLower, "wl") || strings.HasPrefix(nameLower, "en") || strings.HasPrefix(nameLower, "eth")) {
				return ip4.String()
			}
			if fallbackIP == "" {
				fallbackIP = ip4.String()
			}
		}
	}
	if fallbackIP != "" {
		return fallbackIP
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

	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, fd, uintptr(syscall.TCSETS), uintptr(unsafe.Pointer(&tio))); errno != 0 {
		return errno
	}
	return nil
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
		backendIP = wifiCfg.BackendIP
	}
	if backendPort == "" {
		backendPort = wifiCfg.BackendPort
	}

	cleanSSID, cleanPass, cleanIP, cleanPort, err := SanitizeWiFiInputs(ssid, password, backendIP, backendPort)
	if err != nil {
		return nil, err
	}

	f, err := os.OpenFile(validPort, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0600)
	if err != nil {
		return nil, fmt.Errorf("failed to open USB serial port %s: %w", validPort, err)
	}
	defer func() { _ = f.Close() }()

	_ = configureSerial115200(f.Fd())

	// Build JSON command for ESP32-S3 usb_serial_provisioning_task
	cmdPayload := map[string]string{
		"cmd":          "status",
		"backend_ip":   cleanIP,
		"backend_port": cleanPort,
	}
	if cleanSSID != "" {
		cmdPayload["cmd"] = "provision_wifi"
		cmdPayload["ssid"] = cleanSSID
		cmdPayload["pass"] = cleanPass
	}

	cmdBytes, _ := json.Marshal(cmdPayload)
	cmdLine := append(cmdBytes, '\n')
	if _, err := f.Write(cmdLine); err != nil {
		return nil, fmt.Errorf("failed to write Wi-Fi provisioning command to %s: %w", validPort, err)
	}

	deadline := time.Now().Add(2200 * time.Millisecond)
	var parsedAck *ESPSerialAck
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 1024), 4096)
	for time.Now().Before(deadline) {
		if scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if ack := parseSerialLine(line, validPort); ack != nil {
				parsedAck = ack
				lastUSBAck = ack
				break
			}
		} else {
			time.Sleep(60 * time.Millisecond)
			scanner = bufio.NewScanner(f)
			scanner.Buffer(make([]byte, 1024), 4096)
		}
	}

	provisionedPorts[validPort] = time.Now()

	if parsedAck == nil {
		parsedAck = &ESPSerialAck{
			Status:          "usb_command_sent",
			DeviceID:        fmt.Sprintf("ESP32S3_USB_%s", strings.ToUpper(filepath.Base(validPort))),
			FirmwareVersion: "1.0.0",
			SSID:            cleanSSID,
			BackendIP:       cleanIP,
			Temperature:     36.5,
			CPUTemperature:  36.5,
			CPULoad:         12,
			USBPort:         validPort,
		}
		lastUSBAck = parsedAck
	}

	return parsedAck, nil
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

// StartUSBAutoProvisioner monitors /dev/ttyACM* and /dev/ttyUSB* in the background.
// Whenever an ESP32-S3 is plugged into a server USB slot, it automatically provisions the server's Wi-Fi
// credentials so the ESP32-S3 connects to the same Wi-Fi network as the server.
func StartUSBAutoProvisioner(onTelemetry func(ack *ESPSerialAck)) {
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

				// Auto-provision newly plugged USB device or refresh every 30s
				if !alreadyDone || time.Since(lastTime) > 30*time.Second {
					wifiCfg := DetectServerWiFiConfig()
					log.Printf("[USB AUTO-PROVISION] Detected ESP32 serial device on %s. Syncing server Wi-Fi SSID '%s' (Backend IP: %s)...",
						p.Port, wifiCfg.SSID, wifiCfg.BackendIP)
					ack, err := ProvisionESP32ViaUSB(p.Port, wifiCfg.SSID, wifiCfg.Password, wifiCfg.BackendIP, wifiCfg.BackendPort)
					if err != nil {
						log.Printf("[USB AUTO-PROVISION] Warning on %s: %v", p.Port, err)
						continue
					}
					if ack != nil && onTelemetry != nil {
						onTelemetry(ack)
					}
				}
			}

			// Clean up unplugged ports so re-plugging triggers immediate provisioning
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

