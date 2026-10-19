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
}

