package device

import (
	"encoding/json"
	"os"
	"strings"
	"testing"
	"time"
)

func TestDetectServerWiFiConfigAndParseSerial(t *testing.T) {
	// Keep the test hermetic: hide nmcli/iwgetid so the host's live Wi-Fi
	// cannot override the env values under test.
	t.Setenv("PATH", t.TempDir())
	t.Setenv("WIFI_SSID", "BawalFactory_5G")
	t.Setenv("WIFI_PASS", "supersecret123")
	t.Setenv("BACKEND_IP", "192.168.1.50")

	cfg := DetectServerWiFiConfig()
	if cfg.SSID != "BawalFactory_5G" {
		t.Fatalf("expected WIFI_SSID 'BawalFactory_5G', got '%s'", cfg.SSID)
	}
	if !cfg.HasPassword || cfg.Password != "supersecret123" {
		t.Fatalf("expected password to be detected")
	}
	if cfg.BackendIP != "192.168.1.50" {
		t.Fatalf("expected BackendIP '192.168.1.50', got '%s'", cfg.BackendIP)
	}

	// Ensure Password is never leaked in JSON serialization
	marshaled, err := json.Marshal(cfg)
	if err != nil {
		t.Fatalf("json.Marshal failed: %v", err)
	}
	if strings.Contains(string(marshaled), "supersecret123") {
		t.Fatalf("security violation: ServerWiFiConfig leaked password in JSON: %s", string(marshaled))
	}

	ackLine := `I (1042) WIFI_STA: BAWAL_ACK:{"status":"wifi_provisioned","device_id":"ESP32S3_A1B2C3D4E5F6","firmware_version":"1.2.0","ssid":"BawalFactory_5G","backend_ip":"192.168.1.50","temperature":37.25,"cpu_temperature":37.25,"cpu_load":18}`
	ack := parseSerialLine(ackLine, "/dev/ttyACM0")
	if ack == nil {
		t.Fatalf("expected non-nil parsed ACK")
	}
	if ack.DeviceID != "ESP32S3_A1B2C3D4E5F6" {
		t.Fatalf("unexpected DeviceID: %s", ack.DeviceID)
	}
	if ack.Temperature != 37.25 || ack.CPUTemperature != 37.25 || ack.CPULoad != 18 {
		t.Fatalf("unexpected telemetry values in ACK: %+v", ack)
	}
	if ack.USBPort != "/dev/ttyACM0" {
		t.Fatalf("expected USBPort '/dev/ttyACM0', got '%s'", ack.USBPort)
	}

	_ = os.Unsetenv("WIFI_SSID")
	_ = os.Unsetenv("WIFI_PASS")
	_ = os.Unsetenv("BACKEND_IP")
}

func TestReadSerialAckFiltersStaleStatus(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatalf("failed to create pipe: %v", err)
	}
	defer r.Close()

	go func() {
		defer w.Close()
		time.Sleep(30 * time.Millisecond)
		_, _ = w.WriteString("I (100) WIFI_STA: BAWAL_ACK:{\"status\":\"ok\"}\n")
		_, _ = w.WriteString("I (200) WIFI_STA: BAWAL_ACK:{\"status\":\"wifi_provisioned\",\"device_id\":\"ESP32S3_TEST\",\"ssid\":\"HostWiFi\"}\n")
	}()

	ack := readSerialAck(r, time.Now().Add(500*time.Millisecond), "/dev/ttyACM0", func(a *ESPSerialAck) bool {
		return a.Status == "wifi_provisioned" || a.Status == "error"
	})
	if ack == nil {
		t.Fatalf("expected wifi_provisioned ACK")
	}
	if ack.Status != "wifi_provisioned" {
		t.Fatalf("expected stale ok status to be ignored, got %q", ack.Status)
	}
	if ack.DeviceID != "ESP32S3_TEST" {
		t.Fatalf("unexpected DeviceID: %s", ack.DeviceID)
	}
}

func TestUSBSecurityValidation(t *testing.T) {
	// 1. Reject path traversal & non-serial device paths
	maliciousPaths := []string{
		"/etc/passwd",
		"/dev/sda",
		"/dev/ttyACM0/../../etc/shadow",
		"/dev/ttyS0",
		"/tmp/ttyACM0",
		"ttyACM0",
	}
	for _, p := range maliciousPaths {
		if _, err := ValidateUSBPortPath(p); err == nil {
			t.Fatalf("expected ValidateUSBPortPath to reject malicious path %q, but it succeeded", p)
		}
	}

	// 2. Reject control character injection & invalid IPv4/port in Wi-Fi inputs
	if _, _, _, _, err := SanitizeWiFiInputs("MyWiFi\n{\"cmd\":\"evil\"}", "pass1234", "192.168.1.10", "8080"); err == nil {
		t.Fatalf("expected SanitizeWiFiInputs to reject newline injection in SSID")
	}
	if _, _, _, _, err := SanitizeWiFiInputs("MyWiFi", "pass1234", "192.168.1.999; rm -rf /", "8080"); err == nil {
		t.Fatalf("expected SanitizeWiFiInputs to reject invalid backend IPv4")
	}
	if _, _, _, _, err := SanitizeWiFiInputs("MyWiFi", "pass1234", "192.168.1.10", "99999"); err == nil {
		t.Fatalf("expected SanitizeWiFiInputs to reject out-of-range port")
	}

	// 3. Accept valid inputs
	ssid, pass, ip, port, err := SanitizeWiFiInputs("BawalLab_WiFi", "ValidPass#2026", "192.168.1.25", "8080")
	if err != nil {
		t.Fatalf("expected valid Wi-Fi inputs to pass, got error: %v", err)
	}
	if ssid != "BawalLab_WiFi" || pass != "ValidPass#2026" || ip != "192.168.1.25" || port != "8080" {
		t.Fatalf("unexpected sanitized outputs: %s, %s, %s, %s", ssid, pass, ip, port)
	}
}

func TestScanAvailableWiFiNetworks(t *testing.T) {
	networks := ScanAvailableWiFiNetworks()
	if len(networks) == 0 {
		t.Fatalf("expected ScanAvailableWiFiNetworks to return at least 1 network")
	}

	for _, n := range networks {
		t.Logf("Network: %-25s | Signal: %3d%% | Security: %-15s | Server: %5v | Saved: %5v", n.SSID, n.Signal, n.Security, n.IsServer, n.IsSaved)
	}

	foundServer := false
	for _, n := range networks {
		if n.IsServer {
			foundServer = true
			if n.SSID == "" {
				t.Errorf("server network has empty SSID")
			}
			break
		}
	}

	if !foundServer {
		t.Errorf("expected at least one network marked IsServer: true")
	}
}

func TestProbeESP32Security(t *testing.T) {
	// 1. Validate that ProbeESP32ViaUSB rejects non-device / traversal paths
	if _, err := ProbeESP32ViaUSB("../../../etc/shadow"); err == nil {
		t.Fatalf("expected ProbeESP32ViaUSB to reject path traversal")
	}
	if _, err := ProbeESP32ViaUSB("/dev/null"); err == nil {
		t.Fatalf("expected ProbeESP32ViaUSB to reject non-ttyACM/ttyUSB path")
	}

	// 2. Validate parsing of status command ACK containing SSID of a different network
	ackLine := `BAWAL_ACK:{"status":"ok","device_id":"ESP32S3_58CF7912E340","firmware_version":"1.2.0","wifi_connected":true,"ssid":"DifferentWiFi_A","backend_ip":"192.168.100.165","temperature":38.12,"cpu_load":14}`
	ack := parseSerialLine(ackLine, "/dev/ttyACM0")
	if ack == nil {
		t.Fatalf("expected parsed ACK from status response")
	}
	if ack.SSID != "DifferentWiFi_A" {
		t.Fatalf("expected SSID 'DifferentWiFi_A', got '%s'", ack.SSID)
	}
	if !ack.WiFiConnected {
		t.Fatalf("expected WiFiConnected true")
	}
	if ack.Temperature != 38.12 {
		t.Fatalf("expected Temperature 38.12, got %f", ack.Temperature)
	}
}
