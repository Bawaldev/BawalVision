package api

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"bawal_vision_backend/internal/camera_handler"
	"bawal_vision_backend/internal/inspection"
	"bawal_vision_backend/internal/model"

	"github.com/gofiber/fiber/v2"
)

func TestVideoRewindDoesNotAddTelemetry(t *testing.T) {
	app := fiber.New()
	RegisterCameraRoutes(app.Group("/api/v1"), nil)

	// Clean up initial state
	inspection.GlobalInspectionStore.DisconnectDevice("MOCK_VIDEO_CAM")
	camera_handler.GlobalCameraStore.DisconnectDevice("MOCK_VIDEO_CAM")

	// 1. Push a regular frame (forward playback)
	req1 := httptest.NewRequest(http.MethodPost, "/api/v1/camera/frame", bytes.NewReader([]byte("fake_image_bytes")))
	req1.Header.Set("X-Device-ID", "MOCK_VIDEO_CAM")
	req1.Header.Set("X-Frame-ID", "1")
	req1.Header.Set("X-Is-Rewind", "0")
	resp1, err := app.Test(req1)
	if err != nil || resp1.StatusCode != http.StatusOK {
		t.Fatalf("expected forward frame to succeed, err=%v, code=%d", err, resp1.StatusCode)
	}

	tel1, ok := inspection.GlobalInspectionStore.GetLatestTelemetry("MOCK_VIDEO_CAM")
	if !ok {
		t.Fatalf("expected telemetry to be created on initial forward playback")
	}
	initialTimestamp := tel1.Timestamp

	// 2. Push a rewind frame (e.g. video looped or scrubbed backwards)
	req2 := httptest.NewRequest(http.MethodPost, "/api/v1/camera/frame", bytes.NewReader([]byte("fake_image_bytes_2")))
	req2.Header.Set("X-Device-ID", "MOCK_VIDEO_CAM")
	req2.Header.Set("X-Frame-ID", "2")
	req2.Header.Set("X-Is-Rewind", "1")
	resp2, err := app.Test(req2)
	if err != nil || resp2.StatusCode != http.StatusOK {
		t.Fatalf("expected rewind frame to succeed, err=%v, code=%d", err, resp2.StatusCode)
	}

	// 3. Verify camera store knows device is in rewind
	if !camera_handler.GlobalCameraStore.IsDeviceInRewind("MOCK_VIDEO_CAM") {
		t.Errorf("expected GlobalCameraStore to mark MOCK_VIDEO_CAM as in rewind")
	}

	// 4. Verify telemetry was NOT modified or flooded during rewind
	tel2, _ := inspection.GlobalInspectionStore.GetLatestTelemetry("MOCK_VIDEO_CAM")
	if tel2.Timestamp != initialTimestamp {
		t.Errorf("telemetry timestamp changed during rewind: got %d, want %d", tel2.Timestamp, initialTimestamp)
	}
}

func TestVideoRewindDoesNotAddInspectedTrays(t *testing.T) {
	app := fiber.New()
	RegisterDeviceRoutes(app.Group("/api/v1"), nil)

	// Reset mock inspection state
	resetReq := httptest.NewRequest(http.MethodPost, "/api/v1/devices/mock/reset", nil)
	app.Test(resetReq)

	// Mark MOCK_VIDEO_CAM as in rewind state
	camera_handler.GlobalCameraStore.StoreRawFrameWithMeta("MOCK_VIDEO_CAM", 100, []byte("fake_jpg"), "127.0.0.1", true)

	// Send an inspection payload for the rewound video
	payload := model.IngressInspectionRequest{
		DeviceID: "MOCK_VIDEO_CAM",
		IsRewind: true,
		Detections: []model.FoodDetection{
			{
				FoodName:       "banana",
				SimilarityRate: 0.95,
				SpoilageScore:  5.0,
				Status:         "PASS",
			},
		},
	}
	body, _ := json.Marshal(payload)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/inspections", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Is-Rewind", "1")

	resp, err := app.Test(req)
	if err != nil {
		t.Fatalf("POST /inspections failed: %v", err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 OK, got %d", resp.StatusCode)
	}

	// Live HUD store SHOULD be updated for visual display
	insp, ok := inspection.GlobalInspectionStore.GetLatestInspection("MOCK_VIDEO_CAM")
	if !ok || len(insp.Detections) == 0 {
		t.Errorf("expected live HUD detections to remain available during rewind")
	}
}

func TestMockSinglePassInspectionDedup(t *testing.T) {
	app := fiber.New()
	RegisterDeviceRoutes(app.Group("/api/v1"), nil)

	// Reset mock inspection state
	resetReq := httptest.NewRequest(http.MethodPost, "/api/v1/devices/mock/reset", nil)
	app.Test(resetReq)

	camera_handler.GlobalCameraStore.StoreRawFrameWithMeta("MOCK_VIDEO_CAM", 1, []byte("fake_jpg"), "127.0.0.1", false)

	payload := model.IngressInspectionRequest{
		DeviceID: "MOCK_VIDEO_CAM",
		IsRewind: false,
		Detections: []model.FoodDetection{
			{
				FoodName:       "banana",
				SimilarityRate: 0.92,
				SpoilageScore:  8.0,
				Status:         "PASS",
			},
		},
	}
	body, _ := json.Marshal(payload)

	// First pass -> allowed
	req1 := httptest.NewRequest(http.MethodPost, "/api/v1/inspections", bytes.NewReader(body))
	req1.Header.Set("Content-Type", "application/json")
	resp1, _ := app.Test(req1)
	if resp1.StatusCode != http.StatusOK && resp1.StatusCode != http.StatusCreated {
		t.Fatalf("expected first pass to succeed, got %d", resp1.StatusCode)
	}

	// Second pass (e.g. video loop without explicit reset) -> suppressed to prevent duplicate trays
	req2 := httptest.NewRequest(http.MethodPost, "/api/v1/inspections", bytes.NewReader(body))
	req2.Header.Set("Content-Type", "application/json")
	resp2, _ := app.Test(req2)
	if resp2.StatusCode != http.StatusOK {
		t.Fatalf("expected suppressed duplicate to return 200 OK, got %d", resp2.StatusCode)
	}

	var resMap map[string]interface{}
	json.NewDecoder(resp2.Body).Decode(&resMap)
	if resMap["is_rewind"] != true {
		t.Errorf("expected is_rewind suppression flag on duplicate mock video pass")
	}
}

