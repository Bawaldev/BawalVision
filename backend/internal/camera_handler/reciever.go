package camera_handler

import (
	"math"
	"sync"
	"time"
)

type CachedFrame struct {
	DeviceID    string
	FrameID     uint32
	ImageBytes  []byte
	TimestampMS int64
	ClientIP    string
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
	lastFrameTimeMS int64
	frameCount      uint64
	fps             float64
}

var GlobalCameraStore = &CameraStore{
	rawFrames:       make(map[string]CachedFrame),
	annotatedFrames: make(map[string]CachedFrame),
	subscribers:     make(map[uint64]*subscriber),
}

func (s *CameraStore) StoreRawFrame(deviceID string, frameID uint32, data []byte) {
	s.StoreRawFrameWithIP(deviceID, frameID, data, "")
}

func (s *CameraStore) StoreRawFrameWithIP(deviceID string, frameID uint32, data []byte, clientIP string) {
	if deviceID == "" || deviceID == "\\" || deviceID == "/" || deviceID == "null" {
		deviceID = "DEFAULT_CAMERA"
	}
	s.Lock()
	now := time.Now().UnixMilli()
	if s.lastFrameTimeMS > 0 && now > s.lastFrameTimeMS {
		dt := float64(now-s.lastFrameTimeMS) / 1000.0
		if dt > 0.005 && dt < 2.0 {
			instantFps := 1.0 / dt
			if s.fps <= 0 {
				s.fps = instantFps
			} else {
				s.fps = 0.85*s.fps + 0.15*instantFps
			}
		}
	}
	s.lastFrameTimeMS = now
	s.frameCount++

	s.rawFrames[deviceID] = CachedFrame{
		DeviceID:    deviceID,
		FrameID:     frameID,
		ImageBytes:  data,
		TimestampMS: now,
		ClientIP:    clientIP,
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
	now := time.Now().UnixMilli()

	// Try exact device match first
	if deviceID != "" && deviceID != "latest" && deviceID != "UNKNOWN_DEVICE" && deviceID != "*" {
		if f, ok := s.rawFrames[deviceID]; ok && len(f.ImageBytes) > 0 && (now-f.TimestampMS) <= 2500 {
			return f.ImageBytes, true
		}
	}

	// Fallback to latest raw frame across all devices
	var latestFrame *CachedFrame
	for _, f := range s.rawFrames {
		if (now-f.TimestampMS) <= 2500 && (latestFrame == nil || f.TimestampMS > latestFrame.TimestampMS) {
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
	id, _, _, ok := s.GetActiveDeviceMeta()
	if !ok {
		return ""
	}
	return id
}

func (s *CameraStore) GetActiveDeviceMeta() (string, string, uint32, bool) {
	s.RLock()
	defer s.RUnlock()

	now := time.Now().UnixMilli()
	var latest CachedFrame
	var found bool
	for id, f := range s.rawFrames {
		if id != "" && id != "\\" && id != "/" && id != "null" && (now-f.TimestampMS) <= 2500 {
			if !found || f.TimestampMS > latest.TimestampMS {
				latest = f
				found = true
			}
		}
	}
	if !found {
		return "", "", 0, false
	}
	return latest.DeviceID, latest.ClientIP, latest.FrameID, true
}

func (s *CameraStore) DisconnectDevice(deviceID string) {
	s.Lock()
	defer s.Unlock()
	if deviceID == "" || deviceID == "*" {
		s.rawFrames = make(map[string]CachedFrame)
		s.annotatedFrames = make(map[string]CachedFrame)
		return
	}
	delete(s.rawFrames, deviceID)
	delete(s.annotatedFrames, deviceID)
}

func (s *CameraStore) GetActiveFPS() float64 {
	s.RLock()
	defer s.RUnlock()
	now := time.Now().UnixMilli()
	if now-s.lastFrameTimeMS > 2500 || s.fps <= 0 {
		return 0.0
	}
	return math.Round(s.fps*10) / 10
}


