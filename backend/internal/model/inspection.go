package model

import (
	"time"
)

type InspectionFood struct {
	ID             string    `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	InspectionID   string    `json:"inspection_id" gorm:"type:uuid;not null;index"`
	FoodItemID     int       `json:"food_item_id" gorm:"type:int;not null;index"`
	Temperature    float64   `json:"temperature" gorm:"column:temperature;type:numeric(5,2);not null;default:25.00"`
	SimilarityRate float64   `json:"similarity_rate" gorm:"column:similarity_rate;type:numeric(5,4);not null;default:0.0000"`
	SpoilageScore  float64   `json:"spoilage_score" gorm:"column:spoilage_score;type:numeric(5,2);not null;default:0.00"`
	Status         string    `json:"status" gorm:"column:status;type:varchar(16);not null;default:'PASS'"`
	FoodItem       *FoodItem `json:"food_item,omitempty" gorm:"foreignKey:FoodItemID"`
}

func (InspectionFood) TableName() string { return "inspection_foods" }

type InspectionRecord struct {
	ID              string           `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	UserID          *string          `json:"user_id,omitempty" gorm:"type:uuid;index"`
	DeviceID        string           `json:"device_id" gorm:"column:device_id;type:varchar(64);not null"`
	FirmwareVersion string           `json:"firmware_version" gorm:"column:firmware_version;type:varchar(32);not null;default:'1.0.0'"`
	CreatedAt       time.Time        `json:"created_at" gorm:"column:created_at;type:timestamptz;not null;default:NOW()"`
	ImageS3URI      *string          `json:"image_s3_uri,omitempty" gorm:"column:image_s3_uri;type:text"`
	CPUTemperature  float64          `json:"cpu_temperature" gorm:"column:cpu_temperature;type:numeric(5,2);not null;default:35.00"`
	Temperature     float64          `json:"temperature" gorm:"column:temperature;type:numeric(5,2);not null;default:25.00"`
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
	FoodTemperature float64 `json:"food_temperature,omitempty"`
	Temperature     float64 `json:"temperature"`
	CPUTemperature  float64 `json:"cpu_temperature,omitempty"`
	CurrentCPULoad  uint8   `json:"cpu_load"`
	IPAddress       string  `json:"ip_address,omitempty"`
	ConnectionMode  string  `json:"connection_mode,omitempty"`
}

type PredictionResult struct {
	IsSpoiled  bool    `json:"is_spoiled"`
	TSafeHours float64 `json:"t_safe_hours"`
}

type FoodDetection struct {
	FoodName       string  `json:"food_name"`
	SimilarityRate float64 `json:"similarity_rate"`
	Temperature    float64 `json:"temperature,omitempty"`
	SpoilageScore  float64 `json:"spoilage_score,omitempty"`
	Status         string  `json:"status,omitempty"`
	Box            []int   `json:"box,omitempty"`
}

type IngressInspectionRequest struct {
	DeviceID        string          `json:"device_id"`
	FirmwareVersion string          `json:"firmware_version,omitempty"`
	UserID          *string         `json:"user_id,omitempty"`
	FoodTemperature *float64        `json:"food_temperature,omitempty"`
	Temperature     *float64        `json:"temperature,omitempty"`
	CPUTemperature  *float64        `json:"cpu_temperature,omitempty"`
	CPULoad         *uint8          `json:"cpu_load,omitempty"`
	ImageS3URI      *string         `json:"image_s3_uri,omitempty"`
	Detections      []FoodDetection `json:"detections"`
}

