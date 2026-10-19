package inspection

import (
	"bawal_vision_backend/internal/model"
)

// Re-export centralized inspection models for modularity and backward compatibility
type InspectionFood = model.InspectionFood
type InspectionRecord = model.InspectionRecord
type TelemetryIngressPayload = model.TelemetryIngressPayload
type MockTelemetryPayload = model.MockTelemetryPayload
type PredictionResult = model.PredictionResult
type FoodDetection = model.FoodDetection
type SceneOpticalMetrics = model.SceneOpticalMetrics
type IngressInspectionRequest = model.IngressInspectionRequest
type MockInspectionRequest = model.MockInspectionRequest
type MockInspectionRecord = model.MockInspectionRecord

