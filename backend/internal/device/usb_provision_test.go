package device

import (
	"encoding/json"
	"os"
	"strings"
	"testing"
)

func TestDetectServerWiFiConfigAndParseSerial(t *testing.T) {
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

