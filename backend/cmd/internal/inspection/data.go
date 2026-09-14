package inspection

import (
	"sync"
	"time"
)

type TelemetryIngressPayload struct {
	DeviceID    string    `json:"device_id"`
	Timestamp   time.Time `json:"timestamp"`
	Temperature float64   `json:"temperature"`
	Humidity    float64   `json:"humidity"`
	GasVOC      float64   `json:"gas_voc"`
	Pressure    float64   `json:"pressure"`
}

type FoodType struct {
	SimilarityRate float64 `json:"similarity_rate" db:"similarity_rate"`
	TypeName       string  `json:"type_name" db:"type_name"`
}

type SensorData struct {
	Temperature float64   `json:"temperature" db:"temperature"`
	Pressure    float64   `json:"pressure" db:"pressure"`
	Timestamp   time.Time `json:"timestamp"`
}

type PredictionResult struct {
	IsSpoiled  bool    `json:"is_spoiled"`
	TotalGrams float64 `json:"total_grams"`
	TSafeHours float64 `json:"t_safe_hours"`
}

type Food struct {
	Type       FoodType   `json:"type"`
	FoodName   string     `json:"food_name" db:"food_name"`
	SensorData SensorData `json:"sensor_data"`
}

type InspectionRecord struct {
	ID         string                  `json:"id" db:"id"`
	DeviceID   string                  `json:"device_id" db:"device_id"`
	CreatedAt  time.Time               `json:"created_at" db:"created_at"`
	Telemetry  TelemetryIngressPayload `json:"telemetry" db:"telemetry"`
	Foods      []Food                  `json:"foods" db:"foods"`
	Prediction PredictionResult        `json:"prediction" db:"prediction"`
	ImageS3URI string                  `json:"image_s3_uri" db:"image_s3_uri"`
}

type InspectionStore struct {
	sync.RWMutex
	readings map[string]TelemetryIngressPayload
}
