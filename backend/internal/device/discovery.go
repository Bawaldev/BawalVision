package device

import (
	"encoding/json"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
)

const (
	// DefaultDiscoveryPort is the UDP port used by ESP32-S3 nodes to locate
	// the backend when their configured IP is stale or moving.
	DefaultDiscoveryPort = 8765
	discoveryProbePrefix = "BAWAL_DISCOVER"
)

// DiscoveryResponse is the JSON payload sent back to an ESP32-S3 probe.
type DiscoveryResponse struct {
	Service     string `json:"service"`
	Version     string `json:"version"`
	BackendPort int    `json:"backend_port"`
	Hostname    string `json:"hostname,omitempty"`
}

func discoveryPort() int {
	raw := strings.TrimSpace(os.Getenv("DISCOVERY_PORT"))
	if raw == "" {
		return DefaultDiscoveryPort
	}
	port, err := strconv.Atoi(raw)
	if err != nil || port < 1 || port > 65535 {
		log.Printf("[DISCOVERY] Ignoring invalid DISCOVERY_PORT=%q, using %d", raw, DefaultDiscoveryPort)
		return DefaultDiscoveryPort
	}
	return port
}

func backendHTTPPort() int {
	for _, key := range []string{"BACKEND_PORT", "PORT"} {
		raw := strings.TrimSpace(os.Getenv(key))
		if raw == "" {
			continue
		}
		port, err := strconv.Atoi(raw)
		if err == nil && port > 0 && port <= 65535 {
			return port
		}
	}
	return 8080
}

func buildDiscoveryResponse(version string) DiscoveryResponse {
	hostname, _ := os.Hostname()
	return DiscoveryResponse{
		Service:     "bawal-vision",
		Version:     version,
		BackendPort: backendHTTPPort(),
		Hostname:    hostname,
	}
}

// StartBackendDiscovery starts a UDP responder that lets ESP32-S3 edge nodes
// discover the backend's current LAN address after DHCP or Wi-Fi changes.
// It returns immediately; the listener runs in a background goroutine.
func StartBackendDiscovery(version string) {
	port := discoveryPort()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: port})
	if err != nil {
		log.Printf("[DISCOVERY] UDP responder disabled (listen :%d failed: %v)", port, err)
		return
	}

	log.Printf("[DISCOVERY] UDP backend discovery responder listening on :%d (HTTP port %d)", port, backendHTTPPort())

	go func() {
		defer func() {
			_ = conn.Close()
		}()

		buf := make([]byte, 256)
		for {
			n, remoteAddr, err := conn.ReadFromUDP(buf)
			if err != nil {
				log.Printf("[DISCOVERY] UDP read failed: %v", err)
				continue
			}
			probe := strings.TrimSpace(string(buf[:n]))
			if !strings.HasPrefix(probe, discoveryProbePrefix) {
				continue
			}

			response, err := json.Marshal(buildDiscoveryResponse(version))
			if err != nil {
				continue
			}
			if _, err := conn.WriteToUDP(response, remoteAddr); err != nil {
				log.Printf("[DISCOVERY] Failed to reply to %s: %v", remoteAddr, err)
			}
		}
	}()
}
