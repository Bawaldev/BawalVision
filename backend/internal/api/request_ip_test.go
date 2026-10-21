package api

import (
	"io"
	"net"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gofiber/fiber/v2"
)

func requestClientIPForTest(t *testing.T, deviceIPHeader string) string {
	t.Helper()

	app := fiber.New()
	app.Get("/", func(c *fiber.Ctx) error {
		return c.SendString(requestClientIP(c))
	})

	req := httptest.NewRequest("GET", "/", nil)
	if deviceIPHeader != "" {
		req.Header.Set("X-Device-IP", deviceIPHeader)
	}

	resp, err := app.Test(req)
	if err != nil {
		t.Fatalf("request failed: %v", err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read body failed: %v", err)
	}
	return strings.TrimSpace(string(body))
}

func TestRequestClientIPUsesValidDeviceHeader(t *testing.T) {
	if got := requestClientIPForTest(t, "192.168.1.50"); got != "192.168.1.50" {
		t.Fatalf("expected device header IP, got %q", got)
	}
}

func TestRequestClientIPRejectsNonIPDeviceHeader(t *testing.T) {
	got := requestClientIPForTest(t, "application/jso")
	if got == "application/jso" {
		t.Fatal("non-IP device header must not be trusted")
	}
	if net.ParseIP(got) == nil {
		t.Fatalf("fallback is not an IP address: %q", got)
	}
}
