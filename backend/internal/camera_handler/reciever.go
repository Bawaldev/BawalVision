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

type subscriber struct {
	ch       chan []byte
	deviceID string
}

type CameraStore struct {
	sync.RWMutex
	rawFrames       map[string]CachedFrame
	annotatedFrames map[string]CachedFrame
	subscribers     map[uint64]*subscriber
	subCounter      uint64
}

var GlobalCameraStore = &CameraStore{
	rawFrames:       make(map[string]CachedFrame),
	annotatedFrames: make(map[string]CachedFrame),
	subscribers:     make(map[uint64]*subscriber),
}

func (s *CameraStore) StoreRawFrame(deviceID string, frameID uint32, data []byte) {
	s.Lock()
	now := time.Now().UnixMilli()
	s.rawFrames[deviceID] = CachedFrame{
		DeviceID:    deviceID,
		FrameID:     frameID,
		ImageBytes:  data,
		TimestampMS: now,
	}

	// Check if vision service is actively streaming annotated frames (< 2500ms)
	hasActiveAnnotatedStream := false
	for _, f := range s.annotatedFrames {
		if now-f.TimestampMS < 2500 {
			hasActiveAnnotatedStream = true
			break
		}
	}

	// If vision service is active, suppress raw frame broadcast to eliminate alternating box/no-box flicker
	if hasActiveAnnotatedStream {
		s.Unlock()
		return
	}

	subs := make([]chan []byte, 0, len(s.subscribers))
	for _, sub := range s.subscribers {
		isMatch := sub.deviceID == "" || sub.deviceID == deviceID || sub.deviceID == "latest" || sub.deviceID == "*"
		if !isMatch {
			if f, exists := s.rawFrames[sub.deviceID]; !exists || (now-f.TimestampMS > 2500) {
				isMatch = true
			}
		}
		if isMatch {
			subs = append(subs, sub.ch)
		}
	}
	s.Unlock()

	// Non-blocking instantaneous broadcast with stale frame drop
	for _, ch := range subs {
		select {
		case ch <- data:
		default:
			select {
			case <-ch:
			default:
			}
			select {
			case ch <- data:
			default:
			}
		}
	}
}

func (s *CameraStore) StoreAnnotatedFrame(deviceID string, frameID uint32, data []byte) {
	s.Lock()
	now := time.Now().UnixMilli()
	s.annotatedFrames[deviceID] = CachedFrame{
		DeviceID:    deviceID,
		FrameID:     frameID,
		ImageBytes:  data,
		TimestampMS: now,
	}

	subs := make([]chan []byte, 0, len(s.subscribers))
	for _, sub := range s.subscribers {
		isMatch := sub.deviceID == "" || sub.deviceID == deviceID || sub.deviceID == "latest" || sub.deviceID == "*"
		if !isMatch {
			if f, exists := s.annotatedFrames[sub.deviceID]; !exists || (now-f.TimestampMS > 2500) {
				isMatch = true
			}
		}
		if isMatch {
			subs = append(subs, sub.ch)
		}
	}
	s.Unlock()

	for _, ch := range subs {
		select {
		case ch <- data:
		default:
			select {
			case <-ch:
			default:
			}
			select {
			case ch <- data:
			default:
			}
		}
	}
}

func (s *CameraStore) Subscribe(deviceID string) (<-chan []byte, func()) {
	s.Lock()
	defer s.Unlock()

	s.subCounter++
	id := s.subCounter
	ch := make(chan []byte, 1)

	s.subscribers[id] = &subscriber{
		ch:       ch,
		deviceID: deviceID,
	}

	unsubscribe := func() {
		s.Lock()
		defer s.Unlock()
		if sub, ok := s.subscribers[id]; ok {
			delete(s.subscribers, id)
			close(sub.ch)
		}
	}

	return ch, unsubscribe
}

func (s *CameraStore) GetRawFrame(deviceID string) ([]byte, bool) {
	s.RLock()
	defer s.RUnlock()

	if deviceID == "" || deviceID == "latest" || deviceID == "UNKNOWN_DEVICE" || deviceID == "ESP32-S3-BENCH-01" {
		var latestFrame *CachedFrame
		for _, f := range s.rawFrames {
			if latestFrame == nil || f.TimestampMS > latestFrame.TimestampMS {
				curr := f
				latestFrame = &curr
			}
		}
		if latestFrame != nil && len(latestFrame.ImageBytes) > 0 {
			return latestFrame.ImageBytes, true
		}
	}

	frame, exists := s.rawFrames[deviceID]
	if !exists || len(frame.ImageBytes) == 0 {
		var latestFrame *CachedFrame
		for _, f := range s.rawFrames {
			if latestFrame == nil || f.TimestampMS > latestFrame.TimestampMS {
				curr := f
				latestFrame = &curr
			}
		}
		if latestFrame != nil && len(latestFrame.ImageBytes) > 0 {
			return latestFrame.ImageBytes, true
		}
		return nil, false
	}
	return frame.ImageBytes, true
}

func (s *CameraStore) GetStreamFrame(deviceID string) ([]byte, bool) {
	s.RLock()
	defer s.RUnlock()

	now := time.Now().UnixMilli()

	// 1. Try exact device match for fresh annotated frame
	if deviceID != "" && deviceID != "latest" && deviceID != "UNKNOWN_DEVICE" && deviceID != "*" {
		if f, ok := s.annotatedFrames[deviceID]; ok && len(f.ImageBytes) > 0 && (now-f.TimestampMS < 2500) {
			return f.ImageBytes, true
		}
	}

	// 2. Fallback to latest active annotated frame across any device
	var latestAnn *CachedFrame
	for _, f := range s.annotatedFrames {
		if latestAnn == nil || f.TimestampMS > latestAnn.TimestampMS {
			curr := f
			latestAnn = &curr
		}
	}
	if latestAnn != nil && len(latestAnn.ImageBytes) > 0 && (now-latestAnn.TimestampMS < 2500) {
		return latestAnn.ImageBytes, true
	}

	// 3. Fallback to raw frame
	return s.getRawFrameLocked(deviceID)
}

func (s *CameraStore) getRawFrameLocked(deviceID string) ([]byte, bool) {
	// Try exact device match first
	if deviceID != "" && deviceID != "latest" && deviceID != "UNKNOWN_DEVICE" && deviceID != "*" {
		if f, ok := s.rawFrames[deviceID]; ok && len(f.ImageBytes) > 0 {
			return f.ImageBytes, true
		}
	}

	// Fallback to latest raw frame across all devices
	var latestFrame *CachedFrame
	for _, f := range s.rawFrames {
		if latestFrame == nil || f.TimestampMS > latestFrame.TimestampMS {
			curr := f
			latestFrame = &curr
		}
	}
	if latestFrame != nil && len(latestFrame.ImageBytes) > 0 {
		return latestFrame.ImageBytes, true
	}

	return nil, false
}

func (s *CameraStore) GetActiveDeviceID() string {
	s.RLock()
	defer s.RUnlock()

	var latestID string
	var maxTime int64 = 0
	for id, f := range s.rawFrames {
		if f.TimestampMS > maxTime {
			maxTime = f.TimestampMS
			latestID = id
		}
	}
	return latestID
}

