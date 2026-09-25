package camera_handler

import (
	"sync"
	"time"
)

type CachedFrame struct {
	DeviceID    string
	FrameID     uint32
	ImageBytes  []byte
	TimestampMS int64
}

type CameraStore struct {
	sync.RWMutex
	frames map[string]CachedFrame
}

var GlobalCameraStore = &CameraStore{
	frames: make(map[string]CachedFrame),
}

func (s *CameraStore) StoreRawFrame(deviceID string, frameID uint32, data []byte) {
	s.Lock()
	defer s.Unlock()

	s.frames[deviceID] = CachedFrame{
		DeviceID:    deviceID,
		FrameID:     frameID,
		ImageBytes:  data,
		TimestampMS: time.Now().UnixMilli(),
	}
}

func (s *CameraStore) GetRawFrame(deviceID string) ([]byte, bool) {
	s.RLock()
	defer s.RUnlock()

	if deviceID == "" || deviceID == "latest" || deviceID == "UNKNOWN_DEVICE" || deviceID == "ESP32-S3-BENCH-01" {
		var latestFrame *CachedFrame
		for _, f := range s.frames {
			if latestFrame == nil || f.TimestampMS > latestFrame.TimestampMS {
				curr := f
				latestFrame = &curr
			}
		}
		if latestFrame != nil && len(latestFrame.ImageBytes) > 0 {
			return latestFrame.ImageBytes, true
		}
	}

	frame, exists := s.frames[deviceID]
	if !exists {
		return nil, false
	}
	return frame.ImageBytes, true
}

func (s *CameraStore) GetActiveDeviceID() string {
	s.RLock()
	defer s.RUnlock()

	var latestID string
	var maxTime int64 = 0
	for id, f := range s.frames {
		if f.TimestampMS > maxTime {
			maxTime = f.TimestampMS
			latestID = id
		}
	}
	return latestID
}
