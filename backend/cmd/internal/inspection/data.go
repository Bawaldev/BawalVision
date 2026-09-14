package inspection

import (
	"sync"
	"time"
)

type TelemetryIngressPayload struct {
	DeviceID    string    `json:"device_id" gorm:"-"`
	Timestamp   time.Time `json:"timestamp" gorm:"-"`
	Temperature float64   `json:"temperature" gorm:"column:temperature"`
	Humidity    float64   `json:"humidity" gorm:"column:humidity"`
	GasVOC      float64   `json:"gas_voc" gorm:"column:gas_voc"`
	Pressure    float64   `json:"pressure" gorm:"column:pressure"`
}

type FoodType struct {
	SimilarityRate float64 `json:"similarity_rate"`
	TypeName       string  `json:"type_name"`
}

type SensorData struct {
	Temperature float64   `json:"temperature"`
	Pressure    float64   `json:"pressure"`
	Timestamp   time.Time `json:"timestamp"`
}

type PredictionResult struct {
	IsSpoiled  bool    `json:"is_spoiled" gorm:"column:is_spoiled"`
	TotalGrams float64 `json:"total_grams" gorm:"column:total_grams"`
	TSafeHours float64 `json:"t_safe_hours" gorm:"column:t_safe_hours"`
}

type Food struct {
	Type       FoodType   `json:"type"`
	FoodName   string     `json:"food_name"`
	SensorData SensorData `json:"sensor_data"`
}

type InspectionRecord struct {
	ID         string    `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	DeviceID   string    `json:"device_id" gorm:"column:device_id;not null"`
	CreatedAt  time.Time `json:"created_at" gorm:"column:created_at;autoCreateTime"`
	ImageS3URI string    `json:"image_s3_uri" gorm:"column:image_s3_uri;not null"`

	Telemetry  TelemetryIngressPayload `json:"telemetry" gorm:"embedded"`
	Prediction PredictionResult        `json:"prediction" gorm:"embedded"`

	Foods []Food `json:"foods" gorm:"column:foods;type:jsonb;serializer:json;not null;default:'[]'"`
}

// TableName menentukan nama tabel PostgreSQL secara eksplisit
func (InspectionRecord) TableName() string {
	return "inspections"
}

type InspectionStore struct {
	sync.RWMutex
	readings map[string]TelemetryIngressPayload
}
