package inspection

import (
	"encoding/json"
	"fmt"
	"io"
	"time"
)

func NewInspectionStore() *InspectionStore {
	return &InspectionStore{
		readings: make(map[string]TelemetryIngressPayload),
	}
}

func (s *InspectionStore) ProcessTelemetry(r io.Reader) (*TelemetryIngressPayload, error) {
	var payload TelemetryIngressPayload

	decoder := json.NewDecoder(r)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&payload); err != nil {
		return nil, fmt.Errorf("failed to decode JSON payload: %w", err)
	}

	if payload.DeviceID == "" {
		return nil, fmt.Errorf("missing required field: device_id")
	}
	if payload.Timestamp.IsZero() {
		payload.Timestamp = time.Now()
	}

	s.Lock()
	s.readings[payload.DeviceID] = payload
	s.Unlock()

	return &payload, nil
}

func (s *InspectionStore) GetLatestTelemetry(deviceID string) (TelemetryIngressPayload, bool) {
	s.RLock()
	defer s.RUnlock()
	data, exists := s.readings[deviceID]
	return data, exists
}
