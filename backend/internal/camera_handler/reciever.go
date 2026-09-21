package camera_handler

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"sync"
	"time"
)

// CameraStore provides thread-safe in-memory caching of the latest camera frames per device
type CameraStore struct {
	sync.RWMutex
	latestFrames map[string]FrameIngressPayload
}

var (
	// Default global instance for in-memory camera frame caching
	GlobalCameraStore = NewCameraStore()
)

func NewCameraStore() *CameraStore {
	return &CameraStore{
		latestFrames: make(map[string]FrameIngressPayload),
	}
}

// ProcessFrame reads and validates incoming JSON frame payload
func (s *CameraStore) ProcessFrame(r io.Reader) (*FrameIngressPayload, error) {
	var payload FrameIngressPayload

	decoder := json.NewDecoder(r)
	if err := decoder.Decode(&payload); err != nil {
		return nil, fmt.Errorf("failed to decode JSON frame payload: %w", err)
	}

	if payload.DeviceID == "" {
		return nil, fmt.Errorf("missing required field: device_id")
	}
	if payload.TimestampMS == 0 {
		payload.TimestampMS = time.Now().UnixMilli()
	}

	s.Lock()
	s.latestFrames[payload.DeviceID] = payload
	s.Unlock()

	return &payload, nil
}

// StoreLatestFrame updates the in-memory cache for a specific device
func (s *CameraStore) StoreLatestFrame(payload FrameIngressPayload) {
	s.Lock()
	defer s.Unlock()
	s.latestFrames[payload.DeviceID] = payload
}

// GetLatestFrame retrieves the cached frame payload for a device
func (s *CameraStore) GetLatestFrame(deviceID string) (FrameIngressPayload, bool) {
	s.RLock()
	defer s.RUnlock()
	data, exists := s.latestFrames[deviceID]
	return data, exists
}

// GetLatestRawImage extracts decoded binary image bytes from the latest cached frame
func (s *CameraStore) GetLatestRawImage(deviceID string) ([]byte, string, bool) {
	s.RLock()
	defer s.RUnlock()

	frame, exists := s.latestFrames[deviceID]
	if !exists || frame.ImageBase64 == "" {
		return nil, "", false
	}

	rawBytes, err := base64.StdEncoding.DecodeString(frame.ImageBase64)
	if err != nil {
		return nil, "", false
	}

	contentType := "image/jpeg"
	if frame.Format == uint32(FormatRGB888) || frame.Format == uint32(FormatRGB565) {
		contentType = "application/octet-stream"
	}

	return rawBytes, contentType, true
}
