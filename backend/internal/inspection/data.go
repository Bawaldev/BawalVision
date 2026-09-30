package inspection

import (
	"bawal_vision_backend/internal/food"
	"time"
)

// GORM Models (Tabel DB)

type InspectionFood struct {
	ID           string         `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	InspectionID string         `json:"inspection_id" gorm:"type:uuid;not null;index"`
	FoodItemID   int            `json:"food_item_id" gorm:"type:int;not null;index"`
	WeightGrams  float64        `json:"weight_grams" gorm:"column:weight_grams;type:numeric(7,2);not null;default:0.00"`
	FoodItem     *food.FoodItem `json:"food_item,omitempty" gorm:"foreignKey:FoodItemID"`
}

func (InspectionFood) TableName() string { return "inspection_foods" }

type InspectionRecord struct {
	ID              string           `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	UserID          *string          `json:"user_id,omitempty" gorm:"type:uuid;index"`
	DeviceID        string           `json:"device_id" gorm:"column:device_id;type:varchar(64);not null"`
	FirmwareVersion string           `json:"firmware_version" gorm:"column:firmware_version;type:varchar(32);not null;default:'1.0.0'"`
	CreatedAt       time.Time        `json:"created_at" gorm:"column:created_at;type:timestamptz;not null;default:NOW()"`
	ImageS3URI      *string          `json:"image_s3_uri,omitempty" gorm:"column:image_s3_uri;type:text"`
	Temperature     float64          `json:"temperature" gorm:"column:temperature;type:numeric(5,2);not null"`
	CPULoad         uint8            `json:"cpu_load" gorm:"column:cpu_load;type:smallint;not null;default:0"`
	IsSpoiled       bool             `json:"is_spoiled" gorm:"column:is_spoiled;not null;default:false"`
	TSafeHours      float64          `json:"t_safe_hours" gorm:"column:t_safe_hours;type:numeric(5,2);not null;default:0.00"`
	InspectionFoods []InspectionFood `json:"inspection_foods,omitempty" gorm:"foreignKey:InspectionID"`
}

func (InspectionRecord) TableName() string { return "inspections" }

type TelemetryIngressPayload struct {
	DeviceID        string  `json:"device_id"`
	FirmwareVersion string  `json:"firmware_version"`
	DeviceName      string  `json:"device_name"`
	DeviceCore      uint8   `json:"device_core"`
	Timestamp       int64   `json:"timestamp"`
	Temperature     float64 `json:"temperature"`
	CurrentCPULoad  uint8   `json:"cpu_load"`
}

type PredictionResult struct {
	IsSpoiled  bool    `json:"is_spoiled"`
	TotalGrams float64 `json:"total_grams"`
	TSafeHours float64 `json:"t_safe_hours"`
}

type FoodDetection struct {
	FoodName       string  `json:"food_name"`
	SimilarityRate float64 `json:"similarity_rate"`
	WeightGrams    float64 `json:"weight_grams,omitempty"`
}

type IngressInspectionRequest struct {
	DeviceID        string          `json:"device_id"`
	FirmwareVersion string          `json:"firmware_version,omitempty"`
	UserID          *string         `json:"user_id,omitempty"`
	Temperature     *float64        `json:"temperature,omitempty"`
	CPULoad         *uint8          `json:"cpu_load,omitempty"`
	ImageS3URI      *string         `json:"image_s3_uri,omitempty"`
	Detections      []FoodDetection `json:"detections"`
}
