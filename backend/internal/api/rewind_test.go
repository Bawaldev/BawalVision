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
	if err != nil {
		t.Fatalf("expected forward frame to succeed, err=%v", err)
	}
	defer resp1.Body.Close()
	if resp1.StatusCode != http.StatusOK {
		t.Fatalf("expected forward frame status 200 OK, got %d", resp1.StatusCode)
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
	if err != nil {
		t.Fatalf("expected rewind frame to succeed, err=%v", err)
	}
	defer resp2.Body.Close()
	if resp2.StatusCode != http.StatusOK {
		t.Fatalf("expected rewind frame status 200 OK, got %d", resp2.StatusCode)
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
	resetResp, err := app.Test(resetReq)
	if err != nil {
		t.Fatalf("reset mock state failed: %v", err)
	}
	defer resetResp.Body.Close()

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
	defer resp.Body.Close()
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
	resetResp, err := app.Test(resetReq)
	if err != nil {
		t.Fatalf("reset mock state failed: %v", err)
	}
	defer resetResp.Body.Close()

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
	resp1, err := app.Test(req1)
	if err != nil {
		t.Fatalf("first pass request failed: %v", err)
	}
	defer resp1.Body.Close()
	if resp1.StatusCode != http.StatusOK && resp1.StatusCode != http.StatusCreated {
		t.Fatalf("expected first pass to succeed, got %d", resp1.StatusCode)
	}

	// Second pass (e.g. video loop without explicit reset) -> suppressed to prevent duplicate trays
	req2 := httptest.NewRequest(http.MethodPost, "/api/v1/inspections", bytes.NewReader(body))
	req2.Header.Set("Content-Type", "application/json")
	resp2, err := app.Test(req2)
	if err != nil {
		t.Fatalf("second pass request failed: %v", err)
	}
	defer resp2.Body.Close()
	if resp2.StatusCode != http.StatusOK {
		t.Fatalf("expected suppressed duplicate to return 200 OK, got %d", resp2.StatusCode)
	}

	var resMap map[string]interface{}
	if err := json.NewDecoder(resp2.Body).Decode(&resMap); err != nil {
		t.Fatalf("failed to decode response JSON: %v", err)
	}
	if resMap["is_rewind"] != true {
		t.Errorf("expected is_rewind suppression flag on duplicate mock video pass")
	}
}

func TestMockVsLiveStructSeparationAndSharedYoloLogic(t *testing.T) {
	app := fiber.New()
	v1 := app.Group("/api/v1")
	RegisterCameraRoutes(v1, nil)
	RegisterDeviceRoutes(v1, nil)

	// Reset mock state
	resetReq := httptest.NewRequest(http.MethodPost, "/api/v1/devices/mock/reset", nil)
	resetResp, err := app.Test(resetReq)
	if err != nil {
		t.Fatalf("reset mock state failed: %v", err)
	}
	defer resetResp.Body.Close()

	// Shared YOLO detection slice used identically by Mock and Live
	sharedYoloDetections := []model.FoodDetection{
		{
			FoodName:       "banana",
			SimilarityRate: 0.91,
			SpoilageScore:  12.5,
			Status:         "PASS",
			Box:            []int{100, 120, 220, 180},
		},
	}

	// 1. Submit Mock Video Inspection using MockInspectionRequest
	mockPayload := model.MockInspectionRequest{
		MockID:           "MOCK_VIDEO_CAM",
		DeviceID:         "MOCK_VIDEO_CAM",
		SourceType:       "mock_video",
		InspectionStruct: "MockInspectionRequest",
		VideoSource:      "banana/banana_sample_one.mp4",
		EnginePipeline:   "YOLOv8n-INT8 + OpenCV HSV",
		Detections:       sharedYoloDetections,
	}
	mockBody, _ := json.Marshal(mockPayload)
	mockReq := httptest.NewRequest(http.MethodPost, "/api/v1/inspections/mock", bytes.NewReader(mockBody))
	mockReq.Header.Set("Content-Type", "application/json")
	mockReq.Header.Set("X-Stream-Source", "MOCK_VIDEO")
	mockReq.Header.Set("X-Inspection-Struct", "MockInspectionRequest")

	mockResp, err := app.Test(mockReq)
	if err != nil {
		t.Fatalf("mock inspection request failed: %v", err)
	}
	defer mockResp.Body.Close()
	if mockResp.StatusCode != http.StatusCreated {
		t.Fatalf("expected 201 Created for mock inspection, got %d", mockResp.StatusCode)
	}
	if mockResp.Header.Get("X-Inspection-Struct") != "MockInspectionRecord" {
		t.Errorf("expected X-Inspection-Struct=MockInspectionRecord header, got %q", mockResp.Header.Get("X-Inspection-Struct"))
	}
	if mockResp.Header.Get("X-Stream-Source") != "MOCK_VIDEO" {
		t.Errorf("expected X-Stream-Source=MOCK_VIDEO header, got %q", mockResp.Header.Get("X-Stream-Source"))
	}

	var mockResMap map[string]interface{}
	if err := json.NewDecoder(mockResp.Body).Decode(&mockResMap); err != nil {
		t.Fatalf("failed to decode mock response: %v", err)
	}

	// 2. Submit Live ESP32-S3 Inspection using IngressInspectionRequest with the SAME YOLO detections
	temp25 := 25.0
	livePayload := model.IngressInspectionRequest{
		DeviceID:        "ESP32S3_441BF6C95678",
		SourceType:      "live_esp32",
		FirmwareVersion: "1.0.0",
		Temperature:     &temp25,
		Detections:      sharedYoloDetections,
	}
	liveBody, _ := json.Marshal(livePayload)
	liveReq := httptest.NewRequest(http.MethodPost, "/api/v1/inspections", bytes.NewReader(liveBody))
	liveReq.Header.Set("Content-Type", "application/json")
	liveReq.Header.Set("X-Stream-Source", "LIVE_ESP32")
	liveReq.Header.Set("X-Inspection-Struct", "IngressInspectionRequest")

	liveResp, err := app.Test(liveReq)
	if err != nil {
		t.Fatalf("live inspection request failed: %v", err)
	}
	defer liveResp.Body.Close()
	if liveResp.StatusCode != http.StatusCreated {
		t.Fatalf("expected 201 Created for live inspection, got %d", liveResp.StatusCode)
	}
	if liveResp.Header.Get("X-Inspection-Struct") != "InspectionRecord" {
		t.Errorf("expected X-Inspection-Struct=InspectionRecord header, got %q", liveResp.Header.Get("X-Inspection-Struct"))
	}
	if liveResp.Header.Get("X-Stream-Source") != "LIVE_ESP32" {
		t.Errorf("expected X-Stream-Source=LIVE_ESP32 header, got %q", liveResp.Header.Get("X-Stream-Source"))
	}

	var liveResMap map[string]interface{}
	if err := json.NewDecoder(liveResp.Body).Decode(&liveResMap); err != nil {
		t.Fatalf("failed to decode live response: %v", err)
	}

	// 3. Verify structs and API messages differ between Mock and Live
	if mockResMap["inspection_struct"] == liveResMap["inspection_struct"] {
		t.Errorf("expected different inspection_struct between mock (%v) and live (%v)", mockResMap["inspection_struct"], liveResMap["inspection_struct"])
	}
	if mockResMap["telemetry_struct"] == liveResMap["telemetry_struct"] {
		t.Errorf("expected different telemetry_struct between mock (%v) and live (%v)", mockResMap["telemetry_struct"], liveResMap["telemetry_struct"])
	}
	if mockResMap["message"] == liveResMap["message"] {
		t.Errorf("expected different API messages between mock and live")
	}

	// 4. Verify YOLO detection and CV return logic (is_spoiled, t_safe_hours, detected_foods_count) are IDENTICAL
	if mockResMap["is_spoiled"] != liveResMap["is_spoiled"] {
		t.Errorf("expected identical YOLO/CV is_spoiled evaluation: mock=%v vs live=%v", mockResMap["is_spoiled"], liveResMap["is_spoiled"])
	}
	if mockResMap["t_safe_hours"] != liveResMap["t_safe_hours"] {
		t.Errorf("expected identical YOLO/CV t_safe_hours evaluation: mock=%v vs live=%v", mockResMap["t_safe_hours"], liveResMap["t_safe_hours"])
	}
	if mockResMap["detected_foods_count"] != liveResMap["detected_foods_count"] {
		t.Errorf("expected identical detected_foods_count: mock=%v vs live=%v", mockResMap["detected_foods_count"], liveResMap["detected_foods_count"])
	}
}

func TestMockVideoUniqueUUIDNoDuplicateTraysOnReload(t *testing.T) {
	app := fiber.New()
	v1 := app.Group("/api/v1")
	RegisterCameraRoutes(v1, nil)
	RegisterDeviceRoutes(v1, nil)

	// Reset mock state before starting test
	resetReq := httptest.NewRequest(http.MethodPost, "/api/v1/devices/mock/reset", nil)
	resetResp, err := app.Test(resetReq)
	if err != nil {
		t.Fatalf("reset mock state failed: %v", err)
	}
	defer resetResp.Body.Close()

	video1 := "banana/banana_sample_one.mp4"
	video2 := "spinach/spinach_sample_one.mp4"

	// 1. Verify each mock video has a deterministic, unique UUID
	uuid1A := inspection.DeterministicMockVideoUUID(video1)
	uuid1B := inspection.DeterministicMockVideoUUID("/api/v1/examples/file/" + video1)
	uuid2 := inspection.DeterministicMockVideoUUID(video2)

	if uuid1A == "" || uuid2 == "" {
		t.Fatalf("expected non-empty deterministic UUIDs")
	}
	if uuid1A != uuid1B {
		t.Fatalf("expected identical deterministic UUID for normalized video path: %s vs %s", uuid1A, uuid1B)
	}
	if uuid1A == uuid2 {
		t.Fatalf("expected distinct unique UUIDs for different mock videos: %s vs %s", uuid1A, uuid2)
	}

	// 2. First inspection pass of Video 1 -> 201 Created with ID == uuid1A
	pass1Payload := model.MockInspectionRequest{
		DeviceID:    "MOCK_VIDEO_CAM",
		VideoSource: video1,
		Detections: []model.FoodDetection{
			{FoodName: "banana", SimilarityRate: 0.92, SpoilageScore: 10.0, Status: "PASS"},
		},
	}
	pass1Body, _ := json.Marshal(pass1Payload)
	req1 := httptest.NewRequest(http.MethodPost, "/api/v1/inspections/mock", bytes.NewReader(pass1Body))
	req1.Header.Set("Content-Type", "application/json")
	resp1, err := app.Test(req1)
	if err != nil {
		t.Fatalf("video1 pass 1 failed: %v", err)
	}
	defer resp1.Body.Close()
	if resp1.StatusCode != http.StatusCreated {
		t.Fatalf("expected 201 Created on first pass of video1, got %d", resp1.StatusCode)
	}
	var res1 map[string]interface{}
	_ = json.NewDecoder(resp1.Body).Decode(&res1)
	if res1["id"] != uuid1A || res1["video_uuid"] != uuid1A {
		t.Errorf("expected record id & video_uuid to equal %s, got id=%v video_uuid=%v", uuid1A, res1["id"], res1["video_uuid"])
	}

	// 3. Simulate Page Reload on Video 1 (re-sending frame + inspection for Video 1) -> must NOT duplicate tray (returns 200 OK with same UUID)
	reloadReq := httptest.NewRequest(http.MethodPost, "/api/v1/inspections/mock", bytes.NewReader(pass1Body))
	reloadReq.Header.Set("Content-Type", "application/json")
	reloadResp, err := app.Test(reloadReq)
	if err != nil {
		t.Fatalf("video1 reload pass failed: %v", err)
	}
	defer reloadResp.Body.Close()
	if reloadResp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 OK (no duplicate tray) on page reload of video1, got %d", reloadResp.StatusCode)
	}
	var resReload map[string]interface{}
	_ = json.NewDecoder(reloadResp.Body).Decode(&resReload)
	if resReload["id"] != uuid1A || resReload["is_rewind"] != true {
		t.Errorf("expected reload to preserve id=%s and set is_rewind=true, got id=%v is_rewind=%v", uuid1A, resReload["id"], resReload["is_rewind"])
	}

	// 4. Switch to Video 2 -> has a different unique UUID (uuid2), so it records once (201 Created)
	pass2Payload := model.MockInspectionRequest{
		DeviceID:    "MOCK_VIDEO_CAM",
		VideoSource: video2,
		Detections: []model.FoodDetection{
			{FoodName: "spinach", SimilarityRate: 0.89, SpoilageScore: 14.0, Status: "PASS"},
		},
	}
	pass2Body, _ := json.Marshal(pass2Payload)
	req2 := httptest.NewRequest(http.MethodPost, "/api/v1/inspections/mock", bytes.NewReader(pass2Body))
	req2.Header.Set("Content-Type", "application/json")
	resp2, err := app.Test(req2)
	if err != nil {
		t.Fatalf("video2 pass 1 failed: %v", err)
	}
	defer resp2.Body.Close()
	if resp2.StatusCode != http.StatusCreated {
		t.Fatalf("expected 201 Created on first pass of video2, got %d", resp2.StatusCode)
	}
	var res2 map[string]interface{}
	_ = json.NewDecoder(resp2.Body).Decode(&res2)
	if res2["id"] != uuid2 || res2["video_uuid"] != uuid2 {
		t.Errorf("expected video2 record id & video_uuid to equal %s, got id=%v video_uuid=%v", uuid2, res2["id"], res2["video_uuid"])
	}

	// 5. Switch back to Video 1 -> still remembered by its unique UUID (uuid1A), so it does NOT duplicate!
	backTo1Req := httptest.NewRequest(http.MethodPost, "/api/v1/inspections/mock", bytes.NewReader(pass1Body))
	backTo1Req.Header.Set("Content-Type", "application/json")
	backTo1Resp, err := app.Test(backTo1Req)
	if err != nil {
		t.Fatalf("switch back to video1 failed: %v", err)
	}
	defer backTo1Resp.Body.Close()
	if backTo1Resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 OK (no duplicate tray) when switching back to video1, got %d", backTo1Resp.StatusCode)
	}
}

func TestMockInspectionRecordHasNoESPHardwareMetrics(t *testing.T) {
	mockRec := model.InspectionRecord{
		ID:              "test-mock-uuid-9999",
		DeviceID:        "MOCK_VIDEO_CAM",
		FirmwareVersion: "1.0.0",
		CPUTemperature:  42.5,
		Temperature:     40.0,
		CPULoad:         50,
		IsSpoiled:       false,
		TSafeHours:      4.5,
	}

	rawBytes, err := json.Marshal(mockRec)
	if err != nil {
		t.Fatalf("failed to marshal InspectionRecord: %v", err)
	}

	var parsed map[string]interface{}
	if err := json.Unmarshal(rawBytes, &parsed); err != nil {
		t.Fatalf("failed to unmarshal JSON: %v", err)
	}

	for _, forbiddenKey := range []string{"firmware_version", "cpu_temperature", "temperature", "cpu_load"} {
		if val, exists := parsed[forbiddenKey]; exists && val != nil && val != "" && val != float64(0) {
			t.Errorf("expected %q to be omitted for mock inspection record, but found %v", forbiddenKey, val)
		}
	}

	if parsed["inspection_struct"] != "MockInspectionRecord" {
		t.Errorf("expected inspection_struct=MockInspectionRecord, got %v", parsed["inspection_struct"])
	}
	if parsed["source_type"] != "mock_video" {
		t.Errorf("expected source_type=mock_video, got %v", parsed["source_type"])
	}
}

