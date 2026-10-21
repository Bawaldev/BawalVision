package camera_handler

import (
	"math"
	"strings"
	"sync"
	"time"
)

type CachedFrame struct {
	DeviceID    string
	FrameID     uint32
	ImageBytes  []byte
	TimestampMS int64
	ClientIP    string
	IsRewind    bool
	VideoSource string
	VideoUUID   string
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
	streamMode      string // "live" (default) or "mock"
	activeVideoSrc  string
	activeVideoUUID string
}

var GlobalCameraStore = &CameraStore{
	rawFrames:       make(map[string]CachedFrame),
	annotatedFrames: make(map[string]CachedFrame),
	subscribers:     make(map[uint64]*subscriber),
	streamMode:      "live",
}

func (s *CameraStore) SetStreamMode(mode string) {
	s.Lock()
	defer s.Unlock()
	mode = strings.ToLower(strings.TrimSpace(mode))
	if mode != "mock" && mode != "live" {
		mode = "live"
	}
	s.streamMode = mode
	if mode == "mock" {
		// When entering mock mode: ALL LIVE STREAM MUST BE TURNED OFF (wipe real camera caches)
		for id := range s.rawFrames {
			if id != "MOCK_VIDEO_CAM" && !strings.HasPrefix(id, "MOCK_") {
				delete(s.rawFrames, id)
			}
		}
		for id := range s.annotatedFrames {
			if id != "MOCK_VIDEO_CAM" && !strings.HasPrefix(id, "MOCK_") {
				delete(s.annotatedFrames, id)
			}
		}
	} else {
		// When entering live mode: wipe mock frames to resume live camera immediately
		delete(s.rawFrames, "MOCK_VIDEO_CAM")
		delete(s.annotatedFrames, "MOCK_VIDEO_CAM")
		for id := range s.rawFrames {
			if strings.HasPrefix(id, "MOCK_") {
				delete(s.rawFrames, id)
			}
		}
		for id := range s.annotatedFrames {
			if strings.HasPrefix(id, "MOCK_") {
				delete(s.annotatedFrames, id)
			}
		}
	}
}

func (s *CameraStore) SetActiveMockVideoMeta(videoSource, videoUUID string) {
	s.Lock()
	defer s.Unlock()
	if strings.TrimSpace(videoSource) != "" {
		s.activeVideoSrc = strings.TrimSpace(videoSource)
	}
	if strings.TrimSpace(videoUUID) != "" {
		s.activeVideoUUID = strings.TrimSpace(videoUUID)
	}
}

func (s *CameraStore) GetActiveMockVideoMeta() (videoSource string, videoUUID string) {
	s.RLock()
	defer s.RUnlock()
	if f, ok := s.rawFrames["MOCK_VIDEO_CAM"]; ok {
		src := f.VideoSource
		uid := f.VideoUUID
		if src == "" {
			src = s.activeVideoSrc
		}
		if uid == "" {
			uid = s.activeVideoUUID
		}
		return src, uid
	}
	return s.activeVideoSrc, s.activeVideoUUID
}

func (s *CameraStore) GetStreamMode() string {
	s.RLock()
	defer s.RUnlock()
	if s.streamMode == "" {
		return "live"
	}
	return s.streamMode
}

func (s *CameraStore) StoreRawFrame(deviceID string, frameID uint32, data []byte) {
	s.StoreRawFrameWithMockMeta(deviceID, frameID, data, "", false, "", "")
}

func (s *CameraStore) StoreRawFrameWithIP(deviceID string, frameID uint32, data []byte, clientIP string) {
	s.StoreRawFrameWithMockMeta(deviceID, frameID, data, clientIP, false, "", "")
}

func (s *CameraStore) StoreRawFrameWithMeta(deviceID string, frameID uint32, data []byte, clientIP string, isRewind bool) {
	s.StoreRawFrameWithMockMeta(deviceID, frameID, data, clientIP, isRewind, "", "")
}

func (s *CameraStore) StoreRawFrameWithMockMeta(deviceID string, frameID uint32, data []byte, clientIP string, isRewind bool, videoSource string, videoUUID string) {
	if deviceID == "" || deviceID == "\\" || deviceID == "/" || deviceID == "null" {
		deviceID = "DEFAULT_CAMERA"
	}
	s.Lock()

	isMockDevice := deviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(deviceID, "MOCK_")
	currentMode := s.streamMode
	if currentMode == "" {
		currentMode = "live"
	}

	// IF in MOCK mode: ALL LIVE STREAM MUST BE TURNED OFF (drop non-mock camera frames)
	if currentMode == "mock" && !isMockDevice {
		s.Unlock()
		return
	}
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

	if isMockDevice {
		if strings.TrimSpace(videoSource) != "" {
			s.activeVideoSrc = strings.TrimSpace(videoSource)
		} else {
			videoSource = s.activeVideoSrc
		}
		if strings.TrimSpace(videoUUID) != "" {
			s.activeVideoUUID = strings.TrimSpace(videoUUID)
		} else {
			videoUUID = s.activeVideoUUID
		}
	}

	s.rawFrames[deviceID] = CachedFrame{
		DeviceID:    deviceID,
		FrameID:     frameID,
		ImageBytes:  data,
		TimestampMS: now,
		ClientIP:    clientIP,
		IsRewind:    isRewind,
		VideoSource: videoSource,
		VideoUUID:   videoUUID,
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

	// Broadcast while holding the lock. Sends are non-blocking (buffer size 1,
	// drop-oldest), so this critical section stays short. Keeping unsubscribe
	// and send under the same lock eliminates the send-on-closed-channel race:
	// a subscriber can no longer be removed/closed between snapshot and send.
	for _, sub := range s.subscribers {
		isMatch := sub.deviceID == "" || sub.deviceID == deviceID || sub.deviceID == "latest" || sub.deviceID == "*"
		if !isMatch {
			if f, exists := s.rawFrames[sub.deviceID]; !exists || (now-f.TimestampMS > 2500) {
				isMatch = true
			}
		}
		if !isMatch {
			continue
		}
		select {
		case sub.ch <- data:
		default:
			// Drop the oldest queued frame, then publish the newest one.
			select {
			case <-sub.ch:
			default:
			}
			select {
			case sub.ch <- data:
			default:
			}
		}
	}
	s.Unlock()
}

func (s *CameraStore) StoreAnnotatedFrame(deviceID string, frameID uint32, data []byte) {
	if deviceID == "" || deviceID == "\\" || deviceID == "/" || deviceID == "null" {
		deviceID = "DEFAULT_CAMERA"
	}
	s.Lock()

	isMockDevice := deviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(deviceID, "MOCK_")
	currentMode := s.streamMode
	if currentMode == "" {
		currentMode = "live"
	}

	// IF in MOCK mode: ALL LIVE STREAM MUST BE TURNED OFF (drop non-mock frames)
	if currentMode == "mock" && !isMockDevice {
		s.Unlock()
		return
	}

	now := time.Now().UnixMilli()
	s.annotatedFrames[deviceID] = CachedFrame{
		DeviceID:    deviceID,
		FrameID:     frameID,
		ImageBytes:  data,
		TimestampMS: now,
	}

	// Broadcast under the store lock; see StoreRawFrameWithMockMeta for the
	// send-on-closed-channel race this prevents. Sends are non-blocking.
	for _, sub := range s.subscribers {
		isMatch := sub.deviceID == "" || sub.deviceID == deviceID || sub.deviceID == "latest" || sub.deviceID == "*"
		if !isMatch {
			if f, exists := s.annotatedFrames[sub.deviceID]; !exists || (now-f.TimestampMS > 2500) {
				isMatch = true
			}
		}
		if !isMatch {
			continue
		}
		select {
		case sub.ch <- data:
		default:
			select {
			case <-sub.ch:
			default:
			}
			select {
			case sub.ch <- data:
			default:
			}
		}
	}
	s.Unlock()
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

	currentMode := s.streamMode
	if currentMode == "" {
		currentMode = "live"
	}
	isMockDev := deviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(deviceID, "MOCK_")
	isGenericReq := deviceID == "" || deviceID == "latest" || deviceID == "UNKNOWN_DEVICE" || deviceID == "ESP32-S3-BENCH-01"

	// Strict Stream Isolation:
	// In mock mode, a request for a specific real hardware ESP device must NEVER receive mock frames.
	if currentMode == "mock" && !isGenericReq && !isMockDev {
		return nil, false
	}
	// In live mode, a request for a mock video device must NEVER receive live frames.
	if currentMode == "live" && isMockDev {
		return nil, false
	}

	if !isGenericReq {
		if frame, exists := s.rawFrames[deviceID]; exists && len(frame.ImageBytes) > 0 {
			return frame.ImageBytes, true
		}
		return nil, false
	}

	var latestFrame *CachedFrame
	for id, f := range s.rawFrames {
		idIsMock := id == "MOCK_VIDEO_CAM" || strings.HasPrefix(id, "MOCK_")
		if currentMode == "mock" && !idIsMock {
			continue
		}
		if currentMode == "live" && idIsMock {
			continue
		}
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

func (s *CameraStore) GetRawFrameWithRewind(deviceID string) ([]byte, bool, bool) {
	s.RLock()
	defer s.RUnlock()

	currentMode := s.streamMode
	if currentMode == "" {
		currentMode = "live"
	}
	isMockDev := deviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(deviceID, "MOCK_")
	isGenericReq := deviceID == "" || deviceID == "latest" || deviceID == "UNKNOWN_DEVICE" || deviceID == "ESP32-S3-BENCH-01"

	// Strict Stream Isolation:
	if currentMode == "mock" && !isGenericReq && !isMockDev {
		return nil, false, false
	}
	if currentMode == "live" && isMockDev {
		return nil, false, false
	}

	if !isGenericReq {
		if frame, exists := s.rawFrames[deviceID]; exists && len(frame.ImageBytes) > 0 {
			return frame.ImageBytes, frame.IsRewind, true
		}
		return nil, false, false
	}

	var latestFrame *CachedFrame
	for id, f := range s.rawFrames {
		idIsMock := id == "MOCK_VIDEO_CAM" || strings.HasPrefix(id, "MOCK_")
		if currentMode == "mock" && !idIsMock {
			continue
		}
		if currentMode == "live" && idIsMock {
			continue
		}
		if latestFrame == nil || f.TimestampMS > latestFrame.TimestampMS {
			curr := f
			latestFrame = &curr
		}
	}
	if latestFrame != nil && len(latestFrame.ImageBytes) > 0 {
		return latestFrame.ImageBytes, latestFrame.IsRewind, true
	}
	return nil, false, false
}

func (s *CameraStore) IsDeviceInRewind(deviceID string) bool {
	s.RLock()
	defer s.RUnlock()
	if f, exists := s.rawFrames[deviceID]; exists {
		return f.IsRewind
	}
	return false
}

func (s *CameraStore) ResetDeviceRewind(deviceID string) {
	s.Lock()
	defer s.Unlock()
	if f, exists := s.rawFrames[deviceID]; exists {
		f.IsRewind = false
		s.rawFrames[deviceID] = f
	}
}

func (s *CameraStore) ResetAllRewind() {
	s.Lock()
	defer s.Unlock()
	for id, f := range s.rawFrames {
		f.IsRewind = false
		s.rawFrames[id] = f
	}
}

func (s *CameraStore) GetStreamFrame(deviceID string) ([]byte, bool) {
	s.RLock()
	defer s.RUnlock()

	now := time.Now().UnixMilli()
	currentMode := s.streamMode
	if currentMode == "" {
		currentMode = "live"
	}
	isMockDev := deviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(deviceID, "MOCK_")
	isGenericReq := deviceID == "" || deviceID == "latest" || deviceID == "UNKNOWN_DEVICE" || deviceID == "*"

	if currentMode == "mock" && !isGenericReq && !isMockDev {
		return nil, false
	}
	if currentMode == "live" && isMockDev {
		return nil, false
	}

	// 1. Try exact device match for fresh annotated frame
	if !isGenericReq {
		if f, ok := s.annotatedFrames[deviceID]; ok && len(f.ImageBytes) > 0 && (now-f.TimestampMS < 2500) {
			return f.ImageBytes, true
		}
	} else {
		// 2. Fallback to latest active annotated frame across devices matching stream mode
		var latestAnn *CachedFrame
		for id, f := range s.annotatedFrames {
			idIsMock := id == "MOCK_VIDEO_CAM" || strings.HasPrefix(id, "MOCK_")
			if currentMode == "mock" && !idIsMock {
				continue
			}
			if currentMode == "live" && idIsMock {
				continue
			}
			if latestAnn == nil || f.TimestampMS > latestAnn.TimestampMS {
				curr := f
				latestAnn = &curr
			}
		}
		if latestAnn != nil && len(latestAnn.ImageBytes) > 0 && (now-latestAnn.TimestampMS < 2500) {
			return latestAnn.ImageBytes, true
		}
	}

	// 3. Fallback to raw frame
	return s.getRawFrameLocked(deviceID)
}

func (s *CameraStore) getRawFrameLocked(deviceID string) ([]byte, bool) {
	now := time.Now().UnixMilli()
	currentMode := s.streamMode
	if currentMode == "" {
		currentMode = "live"
	}
	isMockDev := deviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(deviceID, "MOCK_")
	isGenericReq := deviceID == "" || deviceID == "latest" || deviceID == "UNKNOWN_DEVICE" || deviceID == "*"

	if currentMode == "mock" && !isGenericReq && !isMockDev {
		return nil, false
	}
	if currentMode == "live" && isMockDev {
		return nil, false
	}

	// Try exact device match first
	if !isGenericReq {
		if f, ok := s.rawFrames[deviceID]; ok && len(f.ImageBytes) > 0 && (now-f.TimestampMS) <= 2500 {
			return f.ImageBytes, true
		}
		return nil, false
	}

	// Fallback to latest raw frame across devices matching stream mode
	var latestFrame *CachedFrame
	for id, f := range s.rawFrames {
		idIsMock := id == "MOCK_VIDEO_CAM" || strings.HasPrefix(id, "MOCK_")
		if currentMode == "mock" && !idIsMock {
			continue
		}
		if currentMode == "live" && idIsMock {
			continue
		}
		if (now-f.TimestampMS) <= 5000 && (latestFrame == nil || f.TimestampMS > latestFrame.TimestampMS) {
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
	currentMode := s.streamMode
	if currentMode == "" {
		currentMode = "live"
	}

	if currentMode == "mock" {
		if f, ok := s.rawFrames["MOCK_VIDEO_CAM"]; ok && (now-f.TimestampMS) <= 5000 {
			return f.DeviceID, f.ClientIP, f.FrameID, true
		}
		// In mock mode, the active device is ALWAYS MOCK_VIDEO_CAM, never an ESP32 hardware device
		return "MOCK_VIDEO_CAM", "127.0.0.1", 0, true
	}

	var latest CachedFrame
	var found bool
	for id, f := range s.rawFrames {
		if id != "" && id != "\\" && id != "/" && id != "null" && (now-f.TimestampMS) <= 5000 {
			isMock := id == "MOCK_VIDEO_CAM" || strings.HasPrefix(id, "MOCK_")
			if isMock {
				continue
			}
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
