package inspection

import (
	"encoding/json"
	"fmt"
	"io"
	"sync"
	"time"
)

var GlobalInspectionStore = NewInspectionStore()

type InspectionStore struct {
	sync.RWMutex
	readings          map[string]TelemetryIngressPayload
	latestInspections map[string]IngressInspectionRequest
}

func NewInspectionStore() *InspectionStore {
	return &InspectionStore{
		readings:          make(map[string]TelemetryIngressPayload),
		latestInspections: make(map[string]IngressInspectionRequest),
	}
}

func (s *InspectionStore) StoreTelemetry(payload TelemetryIngressPayload) {
	if payload.DeviceID == "" {
		return
	}
	if payload.Timestamp == 0 {
		payload.Timestamp = time.Now().Unix()
	}

	s.Lock()
	s.readings[payload.DeviceID] = payload
	s.Unlock()
}

func (s *InspectionStore) StoreLatestInspection(req IngressInspectionRequest) {
	if req.DeviceID == "" {
		return
	}
	s.Lock()
	s.latestInspections[req.DeviceID] = req
	s.Unlock()
}

func (s *InspectionStore) GetLatestInspection(deviceID string) (IngressInspectionRequest, bool) {
	s.RLock()
	defer s.RUnlock()
	if deviceID == "" || deviceID == "latest" || deviceID == "*" {
		for _, insp := range s.latestInspections {
			if len(insp.Detections) > 0 {
				return insp, true
			}
		}
	}
	insp, exists := s.latestInspections[deviceID]
	return insp, exists
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
	data, exists := s.readings[deviceID]
	return data, exists
}

func (s *InspectionStore) GetAnyLatestTelemetry() (TelemetryIngressPayload, bool) {
	s.RLock()
	defer s.RUnlock()
	var latest TelemetryIngressPayload
	var maxTs int64 = -1
	for _, reading := range s.readings {
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
		return
	}
	delete(s.readings, deviceID)
}

