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
	frames      map[string]CachedFrame
	subscribers map[uint64]*subscriber
	subCounter  uint64
}

var GlobalCameraStore = &CameraStore{
	frames:      make(map[string]CachedFrame),
	subscribers: make(map[uint64]*subscriber),
}

func (s *CameraStore) StoreRawFrame(deviceID string, frameID uint32, data []byte) {
	s.Lock()
	s.frames[deviceID] = CachedFrame{
		DeviceID:    deviceID,
		FrameID:     frameID,
		ImageBytes:  data,
		TimestampMS: time.Now().UnixMilli(),
	}

	// Copy subscriber channels to broadcast outside lock
	subs := make([]chan []byte, 0, len(s.subscribers))
	for _, sub := range s.subscribers {
		if sub.deviceID == "" || sub.deviceID == deviceID || sub.deviceID == "latest" || sub.deviceID == "*" {
			subs = append(subs, sub.ch)
		}
	}
	s.Unlock()

	// Non-blocking broadcast with stale frame drop
	for _, ch := range subs {
		select {
		case ch <- data:
		default:
			// Buffer full: drain stale frame immediately and push newest frame
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
	if !exists || len(frame.ImageBytes) == 0 {
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

