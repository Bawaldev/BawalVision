package camera_handler

import (
	"testing"
)

func TestStreamModeSeparation(t *testing.T) {
	GlobalCameraStore.SetStreamMode("live")

	// Store frame from physical camera
	GlobalCameraStore.StoreRawFrameWithMeta("ESP32S3_CAM", 1, []byte("live_frame"), "192.168.1.50", false)
	activeID := GlobalCameraStore.GetActiveDeviceID()
	if activeID != "ESP32S3_CAM" {
		t.Fatalf("expected active device to be ESP32S3_CAM, got: %s", activeID)
	}

	// Switch to mock mode: all live stream from real cameras must be turned off!
	GlobalCameraStore.SetStreamMode("mock")
	if GlobalCameraStore.GetStreamMode() != "mock" {
		t.Fatalf("expected stream mode to be mock, got: %s", GlobalCameraStore.GetStreamMode())
	}

	// Incoming real camera frames must be dropped in mock mode
	GlobalCameraStore.StoreRawFrameWithMeta("ESP32S3_CAM", 2, []byte("live_frame_2"), "192.168.1.50", false)
	if _, ok := GlobalCameraStore.rawFrames["ESP32S3_CAM"]; ok {
		t.Fatalf("expected ESP32S3_CAM to be rejected in mock mode")
	}

	// Mock video frames should be accepted in mock mode
	GlobalCameraStore.StoreRawFrameWithMeta("MOCK_VIDEO_CAM", 10, []byte("mock_frame"), "127.0.0.1", false)
	activeMock := GlobalCameraStore.GetActiveDeviceID()
	if activeMock != "MOCK_VIDEO_CAM" {
		t.Fatalf("expected active device to be MOCK_VIDEO_CAM, got: %s", activeMock)
	}

	// Switch back to live mode: live stream turns on again
	GlobalCameraStore.SetStreamMode("live")
	if GlobalCameraStore.GetStreamMode() != "live" {
		t.Fatalf("expected stream mode to be live, got: %s", GlobalCameraStore.GetStreamMode())
	}

	// Real camera frames should now be accepted again
	GlobalCameraStore.StoreRawFrameWithMeta("ESP32S3_CAM", 3, []byte("live_frame_3"), "192.168.1.50", false)
	activeLive := GlobalCameraStore.GetActiveDeviceID()
	if activeLive != "ESP32S3_CAM" {
		t.Fatalf("expected active device to be ESP32S3_CAM, got: %s", activeLive)
	}

	// In live mode, querying MOCK_VIDEO_CAM must NOT return live frames
	if _, ok := GlobalCameraStore.GetRawFrame("MOCK_VIDEO_CAM"); ok {
		t.Fatalf("expected GetRawFrame(MOCK_VIDEO_CAM) to return false in live mode")
	}

	// Switch back to mock mode
	GlobalCameraStore.SetStreamMode("mock")
	GlobalCameraStore.StoreRawFrameWithMeta("MOCK_VIDEO_CAM", 11, []byte("mock_banana"), "127.0.0.1", false)

	// Querying specific ESP32S3_CAM in mock mode must NOT return mock frames
	if _, ok := GlobalCameraStore.GetRawFrame("ESP32S3_CAM"); ok {
		t.Fatalf("expected GetRawFrame(ESP32S3_CAM) to return false in mock mode to prevent cross-contamination")
	}

	// Querying mock device or generic latest must succeed
	if frame, ok := GlobalCameraStore.GetRawFrame("MOCK_VIDEO_CAM"); !ok || string(frame) != "mock_banana" {
		t.Fatalf("expected GetRawFrame(MOCK_VIDEO_CAM) to return mock_banana, got %s", string(frame))
	}
	if frame, ok := GlobalCameraStore.GetRawFrame("latest"); !ok || string(frame) != "mock_banana" {
		t.Fatalf("expected GetRawFrame(latest) to return mock_banana, got %s", string(frame))
	}
}

