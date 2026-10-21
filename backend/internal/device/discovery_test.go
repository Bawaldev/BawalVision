package device

import "testing"

func TestBuildDiscoveryResponse(t *testing.T) {
	t.Setenv("BACKEND_PORT", "9090")

	resp := buildDiscoveryResponse("test-version")
	if resp.Service != "bawal-vision" {
		t.Fatalf("unexpected service: %q", resp.Service)
	}
	if resp.Version != "test-version" {
		t.Fatalf("unexpected version: %q", resp.Version)
	}
	if resp.BackendPort != 9090 {
		t.Fatalf("unexpected backend port: %d", resp.BackendPort)
	}
}

func TestDiscoveryPortValidation(t *testing.T) {
	t.Setenv("DISCOVERY_PORT", "9999")
	if got := discoveryPort(); got != 9999 {
		t.Fatalf("expected 9999, got %d", got)
	}

	t.Setenv("DISCOVERY_PORT", "not-a-port")
	if got := discoveryPort(); got != DefaultDiscoveryPort {
		t.Fatalf("expected default %d, got %d", DefaultDiscoveryPort, got)
	}
}

func TestBackendHTTPPortFallback(t *testing.T) {
	t.Setenv("BACKEND_PORT", "")
	t.Setenv("PORT", "7070")
	if got := backendHTTPPort(); got != 7070 {
		t.Fatalf("expected 7070, got %d", got)
	}

	t.Setenv("PORT", "")
	if got := backendHTTPPort(); got != 8080 {
		t.Fatalf("expected default 8080, got %d", got)
	}
}

func TestUSBAutoProvisionEnabled(t *testing.T) {
	t.Setenv("USB_AUTO_PROVISION", "0")
	if usbAutoProvisionEnabled() {
		t.Fatal("expected USB auto-provision to be disabled")
	}

	t.Setenv("USB_AUTO_PROVISION", "1")
	if !usbAutoProvisionEnabled() {
		t.Fatal("expected USB auto-provision to be enabled")
	}
}
