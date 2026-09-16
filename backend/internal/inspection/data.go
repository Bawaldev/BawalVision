package inspection

import (
	"time"
)

type TelemetryIngressPayload struct {
	DeviceID       string  `json:"device_id" gorm:"-"`
	Timestamp      int64   `json:"timestamp" gorm:"-"`
	Temperature    float64 `json:"temperature" gorm:"column:temperature;type:numeric(5,2);not null"`
	Humidity       float64 `json:"humidity" gorm:"column:humidity;type:numeric(5,2);not null"`
	GasVOC         float64 `json:"gas_voc" gorm:"column:gas_voc;type:numeric(10,2);not null"`
	Pressure       float64 `json:"pressure" gorm:"column:pressure;type:numeric(7,2);not null"`
	CurrentCPULoad uint8   `json:"cpu_load" gorm:"column:cpu_load;type:smallint;not null;default:0"`
}

type PredictionResult struct {
	IsSpoiled  bool    `json:"is_spoiled" gorm:"column:is_spoiled;not null;default:false"`
	TotalGrams float64 `json:"total_grams" gorm:"column:total_grams;type:numeric(7,2);not null;default:0"`
	TSafeHours float64 `json:"t_safe_hours" gorm:"column:t_safe_hours;type:numeric(5,2);not null;default:0"`
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

type Food struct {
	Type       FoodType   `json:"type"`
	FoodName   string     `json:"food_name"`
	SensorData SensorData `json:"sensor_data"`
}

type InspectionRecord struct {
	ID         string    `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	UserID     string    `json:"user_id" gorm:"type:uuid;index"`
	DeviceID   string    `json:"device_id" gorm:"column:device_id;type:varchar(64);not null"`
	CreatedAt  time.Time `json:"created_at" gorm:"column:created_at;type:timestamptz;not null;default:now()"`
	ImageS3URI string    `json:"image_s3_uri" gorm:"column:image_s3_uri;type:text"`

	Telemetry  TelemetryIngressPayload `json:"telemetry" gorm:"embedded"`
	Prediction PredictionResult        `json:"prediction" gorm:"embedded"`

	Foods []Food `json:"foods" gorm:"column:foods;type:jsonb;serializer:json;not null;default:'[]'"`
}

func (InspectionRecord) TableName() string {
	return "inspections"
}
