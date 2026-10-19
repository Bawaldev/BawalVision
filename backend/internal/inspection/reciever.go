package inspection

import (
	"encoding/json"
	"fmt"
	"io"
	"strings"
	"sync"
	"time"
)

var GlobalInspectionStore = NewInspectionStore()

type InspectionStore struct {
	sync.RWMutex
	readings              map[string]TelemetryIngressPayload
	mockReadings          map[string]MockTelemetryPayload
	latestInspections     map[string]IngressInspectionRequest
	latestMockInspections map[string]MockInspectionRequest
}

func NewInspectionStore() *InspectionStore {
	return &InspectionStore{
		readings:              make(map[string]TelemetryIngressPayload),
		mockReadings:          make(map[string]MockTelemetryPayload),
		latestInspections:     make(map[string]IngressInspectionRequest),
		latestMockInspections: make(map[string]MockInspectionRequest),
	}
}

func (s *InspectionStore) StoreTelemetry(payload TelemetryIngressPayload) {
	if payload.DeviceID == "" || payload.DeviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(payload.DeviceID, "MOCK_") {
		return
	}
	nowUnix := time.Now().Unix()
	if payload.Timestamp <= 0 || payload.Timestamp < 1700000000 || (nowUnix-payload.Timestamp) > 30 {
		payload.Timestamp = nowUnix
	}

	s.Lock()
	s.readings[payload.DeviceID] = payload
	s.Unlock()
}

func (s *InspectionStore) StoreMockTelemetry(payload MockTelemetryPayload) {
	devID := payload.DeviceID
	if devID == "" {
		devID = payload.MockID
	}
	if devID == "" {
		return
	}
	payload.DeviceID = devID
	if payload.MockID == "" {
		payload.MockID = devID
	}
	if payload.SourceType == "" {
		payload.SourceType = "mock_video"
	}
	if payload.TelemetryStruct == "" {
		payload.TelemetryStruct = "MockTelemetryPayload"
	}
	if payload.ConnectionMode == "" {
		payload.ConnectionMode = "mock_simulation"
	}
	if payload.EnginePipeline == "" {
		payload.EnginePipeline = "YOLOv8n-INT8 + OpenCV HSV"
	}
	nowUnix := time.Now().Unix()
	if payload.Timestamp <= 0 || payload.Timestamp < 1700000000 || (nowUnix-payload.Timestamp) > 30 {
		payload.Timestamp = nowUnix
	}

	s.Lock()
	s.mockReadings[devID] = payload
	s.Unlock()
}

func (s *InspectionStore) GetLatestMockTelemetry(deviceID string) (MockTelemetryPayload, bool) {
	s.RLock()
	defer s.RUnlock()
	if deviceID == "" || deviceID == "*" || deviceID == "latest" {
		for _, m := range s.mockReadings {
			return m, true
		}
	}
	data, exists := s.mockReadings[deviceID]
	return data, exists
}

func (s *InspectionStore) StoreLatestInspection(req IngressInspectionRequest) {
	if req.DeviceID == "" {
		return
	}
	if req.SourceType == "" {
		req.SourceType = "live_esp32"
	}
	s.Lock()
	s.latestInspections[req.DeviceID] = req
	s.Unlock()
}

func (s *InspectionStore) StoreLatestMockInspection(req MockInspectionRequest) {
	devID := req.DeviceID
	if devID == "" {
		devID = req.MockID
	}
	if devID == "" {
		return
	}
	req.DeviceID = devID
	if req.MockID == "" {
		req.MockID = devID
	}
	if req.SourceType == "" {
		req.SourceType = "mock_video"
	}
	if req.InspectionStruct == "" {
		req.InspectionStruct = "MockInspectionRequest"
	}
	if req.EnginePipeline == "" {
		req.EnginePipeline = "YOLOv8n-INT8 + OpenCV HSV"
	}
	s.Lock()
	s.latestMockInspections[devID] = req
	s.Unlock()
}

func (s *InspectionStore) GetLatestMockInspection(deviceID string) (MockInspectionRequest, bool) {
	s.RLock()
	defer s.RUnlock()
	if deviceID == "" || deviceID == "latest" || deviceID == "*" {
		for _, insp := range s.latestMockInspections {
			if len(insp.Detections) > 0 {
				return insp, true
			}
		}
	}
	insp, exists := s.latestMockInspections[deviceID]
	return insp, exists
}

func (s *InspectionStore) GetLatestInspection(deviceID string) (IngressInspectionRequest, bool) {
	s.RLock()
	defer s.RUnlock()
	if deviceID == "" || deviceID == "latest" || deviceID == "*" {
		for id, insp := range s.latestInspections {
			if id == "MOCK_VIDEO_CAM" || strings.HasPrefix(id, "MOCK_") {
				continue
			}
			if len(insp.Detections) > 0 {
				return insp, true
			}
		}
		return IngressInspectionRequest{}, false
	}
	if insp, exists := s.latestInspections[deviceID]; exists {
		return insp, true
	}
	if mockInsp, exists := s.latestMockInspections[deviceID]; exists {
		return IngressInspectionRequest{
			DeviceID:     mockInsp.DeviceID,
			SourceType:   mockInsp.SourceType,
			SceneOptical: mockInsp.SceneOptical,
			Detections:   mockInsp.Detections,
			IsRewind:     mockInsp.IsRewind,
		}, true
	}
	return IngressInspectionRequest{}, false
}

func (s *InspectionStore) ProcessTelemetry(r io.Reader) (*TelemetryIngressPayload, error) {
	var payload TelemetryIngressPayload

	decoder := json.NewDecoder(r)
	if err := decoder.Decode(&payload); err != nil {
		return nil, fmt.Errorf("failed to decode JSON payload: %w", err)
	}

	if payload.DeviceID == "" {
		return nil, fmt.Errorf("missing required field: device_id")
	}
	s.StoreTelemetry(payload)

	return &payload, nil
}

func (s *InspectionStore) GetLatestTelemetry(deviceID string) (TelemetryIngressPayload, bool) {
	s.RLock()
	defer s.RUnlock()
	if data, exists := s.readings[deviceID]; exists {
		return data, true
	}
	if mockData, exists := s.mockReadings[deviceID]; exists {
		return TelemetryIngressPayload{
			DeviceID:       mockData.DeviceID,
			Timestamp:      mockData.Timestamp,
			ConnectionMode: mockData.ConnectionMode,
			SceneOptical:   mockData.SceneOptical,
		}, true
	}
	return TelemetryIngressPayload{}, false
}

func (s *InspectionStore) GetAnyLatestTelemetry() (TelemetryIngressPayload, bool) {
	s.RLock()
	defer s.RUnlock()
	var latest TelemetryIngressPayload
	var maxTs int64 = -1
	for id, reading := range s.readings {
		if id == "MOCK_VIDEO_CAM" || strings.HasPrefix(id, "MOCK_") {
			continue
		}
		if reading.Timestamp >= maxTs {
			maxTs = reading.Timestamp
			latest = reading
		}
	}
	if maxTs >= 0 {
		return latest, true
	}
	return latest, false
}

func (s *InspectionStore) DisconnectDevice(deviceID string) {
	s.Lock()
	defer s.Unlock()
	if deviceID == "" || deviceID == "*" {
		s.readings = make(map[string]TelemetryIngressPayload)
		s.mockReadings = make(map[string]MockTelemetryPayload)
		s.latestInspections = make(map[string]IngressInspectionRequest)
		s.latestMockInspections = make(map[string]MockInspectionRequest)
		return
	}
	delete(s.readings, deviceID)
	delete(s.mockReadings, deviceID)
	delete(s.latestInspections, deviceID)
	delete(s.latestMockInspections, deviceID)
}

func (s *InspectionStore) DisconnectNonMock() {
	s.Lock()
	defer s.Unlock()
	for id := range s.readings {
		if id != "MOCK_VIDEO_CAM" && !strings.HasPrefix(id, "MOCK_") {
			delete(s.readings, id)
		}
	}
	for id := range s.latestInspections {
		if id != "MOCK_VIDEO_CAM" && !strings.HasPrefix(id, "MOCK_") {
			delete(s.latestInspections, id)
		}
	}
}

// ClearAllDetections flushes all active bounding boxes and detections across both live and mock streams
func (s *InspectionStore) ClearAllDetections() {
	s.Lock()
	defer s.Unlock()
	s.latestInspections = make(map[string]IngressInspectionRequest)
	s.latestMockInspections = make(map[string]MockInspectionRequest)
}


