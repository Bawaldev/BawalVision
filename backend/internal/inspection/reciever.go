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
	readings map[string]TelemetryIngressPayload
}

func NewInspectionStore() *InspectionStore {
	return &InspectionStore{
		readings: make(map[string]TelemetryIngressPayload),
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
