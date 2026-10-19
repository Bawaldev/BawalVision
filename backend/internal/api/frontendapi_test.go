package api

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/gofiber/fiber/v2"
)

func TestGetExampleVideosAndServe(t *testing.T) {
	// Create temporary mock directory structure
	tmpDir, err := os.MkdirTemp("", "bawal_examples_test_*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	subDir := filepath.Join(tmpDir, "banana")
	if err := os.MkdirAll(subDir, 0755); err != nil {
		t.Fatalf("failed to create sub dir: %v", err)
	}

	testFile := filepath.Join(subDir, "banana_sample_one.mp4")
	if err := os.WriteFile(testFile, []byte("fake mp4 content"), 0644); err != nil {
		t.Fatalf("failed to write test file: %v", err)
	}

	app := fiber.New()
	h := &Handler{}

	app.Get("/api/v1/examples", h.GetExampleVideos)
	app.Get("/api/v1/examples/file/*", h.ServeExampleVideo)

	// Test GET /api/v1/examples
	req := httptest.NewRequest(http.MethodGet, "/api/v1/examples", nil)
	resp, err := app.Test(req)
	if err != nil {
		t.Fatalf("GET /api/v1/examples failed: %v", err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 OK, got %d", resp.StatusCode)
	}

	body, _ := io.ReadAll(resp.Body)
	var result struct {
		Status string   `json:"status"`
		Videos []string `json:"videos"`
	}
	if err := json.Unmarshal(body, &result); err != nil {
		t.Fatalf("failed to unmarshal JSON: %v", err)
	}

	// Verify that relative paths in actual repo (banana/banana_sample_one.mp4) are found
	foundBanana := false
	for _, v := range result.Videos {
		if v == "banana/banana_sample_one.mp4" {
			foundBanana = true
			break
		}
	}
	if !foundBanana {
		t.Errorf("expected to find banana/banana_sample_one.mp4 in %v", result.Videos)
	}

	// Test ServeExampleVideo for valid file
	fileReq := httptest.NewRequest(http.MethodGet, "/api/v1/examples/file/banana/banana_sample_one.mp4", nil)
	fileResp, err := app.Test(fileReq)
	if err != nil {
		t.Fatalf("failed to request file: %v", err)
	}
	if fileResp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 OK when requesting valid example file, got %d", fileResp.StatusCode)
	}
	fileBody, _ := io.ReadAll(fileResp.Body)
	if len(fileBody) == 0 {
		t.Errorf("expected non-empty video file body")
	}

	// Test ServeExampleVideo with path traversal attack
	badReq := httptest.NewRequest(http.MethodGet, "/api/v1/examples/file/../../etc/passwd", nil)
	badResp, err := app.Test(badReq)
	if err != nil {
		t.Fatalf("failed bad request: %v", err)
	}
	if badResp.StatusCode != http.StatusBadRequest && badResp.StatusCode != http.StatusNotFound {
		t.Errorf("expected 400 or 404 for path traversal attempt, got %d", badResp.StatusCode)
	}
}
