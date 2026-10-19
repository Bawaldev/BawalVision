package model

import (
	"encoding/json"
	"strings"
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
	ID                 string           `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	DeviceID           string           `json:"device_id" gorm:"column:device_id;type:varchar(64);not null"`
	FirmwareVersion    string           `json:"firmware_version,omitempty" gorm:"column:firmware_version;type:varchar(32);not null;default:'1.0.0'"`
	CreatedAt          time.Time        `json:"created_at" gorm:"column:created_at;type:timestamptz;not null;default:NOW()"`
	ImageS3URI         *string          `json:"image_s3_uri,omitempty" gorm:"column:image_s3_uri;type:text"`
	CPUTemperature     float64          `json:"cpu_temperature,omitempty" gorm:"column:cpu_temperature;type:numeric(5,2);not null;default:35.00"`
	Temperature        float64          `json:"temperature,omitempty" gorm:"column:temperature;type:numeric(5,2);not null;default:25.00"`
	CPULoad            uint8            `json:"cpu_load,omitempty" gorm:"column:cpu_load;type:smallint;not null;default:0"`
	IsSpoiled          bool             `json:"is_spoiled" gorm:"column:is_spoiled;not null;default:false"`
	TSafeHours         float64          `json:"t_safe_hours" gorm:"column:t_safe_hours;type:numeric(5,2);not null;default:0.00"`
	LuxLuminance       float64          `json:"lux_luminance" gorm:"column:lux_luminance;type:numeric(5,2);not null;default:120.00"`
	WhiteBalanceStatus string           `json:"white_balance_status" gorm:"column:white_balance_status;type:varchar(32);not null;default:'BALANCED'"`
	ColorTemperature   int              `json:"color_temperature" gorm:"column:color_temperature;type:int;not null;default:6500"`
	InspectionFoods    []InspectionFood `json:"inspection_foods,omitempty" gorm:"foreignKey:InspectionID"`
}

func (InspectionRecord) TableName() string { return "inspections" }

// MarshalJSON automatically delegates serialization to MockInspectionRecord whenever the record
// originated from a mock video stream or virtual camera. This guarantees mock video records never
// leak physical ESP32-S3 hardware metrics (firmware_version, cpu_temperature, temperature, cpu_load).
func (r InspectionRecord) MarshalJSON() ([]byte, error) {
	if r.DeviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(r.DeviceID, "MOCK_") || (r.ImageS3URI != nil && strings.Contains(*r.ImageS3URI, ".mp4")) {
		return json.Marshal(r.ToMockInspectionRecord(""))
	}
	type Alias InspectionRecord
	return json.Marshal((Alias)(r))
}

type SceneOpticalMetrics struct {
	AvgR               float64 `json:"avg_r"`
	AvgG               float64 `json:"avg_g"`
	AvgB               float64 `json:"avg_b"`
	LuxLuminance       float64 `json:"lux_luminance"`
	ColorTemperature   int     `json:"color_temperature"`
	WhiteBalanceStatus string  `json:"white_balance_status"`
}

type TelemetryIngressPayload struct {
	DeviceID        string               `json:"device_id"`
	FirmwareVersion string               `json:"firmware_version"`
	DeviceName      string               `json:"device_name"`
	DeviceCore      uint8                `json:"device_core"`
	Timestamp       int64                `json:"timestamp"`
	FoodTemperature float64              `json:"food_temperature,omitempty"`
	Temperature     float64              `json:"temperature"`
	CPUTemperature  float64              `json:"cpu_temperature,omitempty"`
	CurrentCPULoad  uint8                `json:"cpu_load"`
	CPUCore0Load    *uint8               `json:"cpu_core0_load,omitempty"`
	CPUCore1Load    *uint8               `json:"cpu_core1_load,omitempty"`
	IPAddress       string               `json:"ip_address,omitempty"`
	ConnectionMode  string               `json:"connection_mode,omitempty"`
	FreeHeap        uint32               `json:"free_heap,omitempty"`
	HeapFree        uint32               `json:"heap_free,omitempty"`
	RSSI            int                  `json:"rssi,omitempty"`
	CameraModel     string               `json:"camera_model,omitempty"`
	SceneOptical    *SceneOpticalMetrics `json:"scene_optical,omitempty"`
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
	DeviceID        string               `json:"device_id"`
	SourceType      string               `json:"source_type,omitempty"`
	FirmwareVersion string               `json:"firmware_version,omitempty"`
	FoodTemperature *float64             `json:"food_temperature,omitempty"`
	Temperature     *float64             `json:"temperature,omitempty"`
	CPUTemperature  *float64             `json:"cpu_temperature,omitempty"`
	CPULoad         *uint8               `json:"cpu_load,omitempty"`
	ImageS3URI      *string              `json:"image_s3_uri,omitempty"`
	SceneOptical    *SceneOpticalMetrics `json:"scene_optical,omitempty"`
	Detections      []FoodDetection      `json:"detections"`
	IsRewind        bool                 `json:"is_rewind,omitempty"`
}

// MockTelemetryPayload is a dedicated telemetry struct for Mock Video streams.
// Unlike TelemetryIngressPayload (which carries physical ESP32-S3 MCU temperatures, CPU load, and RSSI),
// MockTelemetryPayload represents simulation metadata and shared computer vision optical metrics.
type MockTelemetryPayload struct {
	MockID          string               `json:"mock_id"`
	VideoUUID       string               `json:"video_uuid,omitempty"`
	DeviceID        string               `json:"device_id"`
	SourceType      string               `json:"source_type"`      // "mock_video"
	TelemetryStruct string               `json:"telemetry_struct"` // "MockTelemetryPayload"
	VideoSource     string               `json:"video_source,omitempty"`
	FrameID         uint32               `json:"frame_id"`
	Timestamp       int64                `json:"timestamp"`
	IsRewind        bool                 `json:"is_rewind"`
	ConnectionMode  string               `json:"connection_mode"`  // "mock_simulation"
	EnginePipeline  string               `json:"engine_pipeline"`  // "YOLOv8n-INT8 + OpenCV HSV"
	SceneOptical    *SceneOpticalMetrics `json:"scene_optical,omitempty"`
}

// MockInspectionRequest is a dedicated inspection ingress struct for Mock Video streams.
// It shares the exact same SceneOpticalMetrics and []FoodDetection YOLO/CV structures as live streams,
// but omits physical ESP32-S3 hardware sensor fields (firmware_version, cpu_temperature, cpu_load).
type MockInspectionRequest struct {
	MockID           string               `json:"mock_id"`
	VideoUUID        string               `json:"video_uuid,omitempty"`
	DeviceID         string               `json:"device_id"`
	SourceType       string               `json:"source_type"`       // "mock_video"
	InspectionStruct string               `json:"inspection_struct"` // "MockInspectionRequest"
	VideoSource      string               `json:"video_source,omitempty"`
	FrameID          uint32               `json:"frame_id,omitempty"`
	IsRewind         bool                 `json:"is_rewind,omitempty"`
	EnginePipeline   string               `json:"engine_pipeline,omitempty"`
	SceneOptical     *SceneOpticalMetrics `json:"scene_optical,omitempty"`
	Detections       []FoodDetection      `json:"detections"`
}

// MockInspectionRecord represents the persisted/returned inspection audit struct for Mock Video runs.
// It uses the exact same YOLO FoodDetection, InspectionFood, and spoilage/shelf-life return logic as InspectionRecord,
// while structurally differentiating mock video runs from physical ESP32-S3 hardware audits.
type MockInspectionRecord struct {
	ID                 string               `json:"id"`
	MockID             string               `json:"mock_id"`
	VideoUUID          string               `json:"video_uuid"`
	DeviceID           string               `json:"device_id"`
	SourceType         string               `json:"source_type"`       // "mock_video"
	InspectionStruct   string               `json:"inspection_struct"` // "MockInspectionRecord"
	VideoSource        string               `json:"video_source,omitempty"`
	ImageS3URI         *string              `json:"image_s3_uri,omitempty"`
	EnginePipeline     string               `json:"engine_pipeline"`   // "YOLOv8n-INT8 + OpenCV HSV"
	CreatedAt          time.Time            `json:"created_at"`
	IsSpoiled          bool                 `json:"is_spoiled"`
	TSafeHours         float64              `json:"t_safe_hours"`
	LuxLuminance       float64              `json:"lux_luminance"`
	WhiteBalanceStatus string               `json:"white_balance_status"`
	ColorTemperature   int                  `json:"color_temperature"`
	SceneOptical       *SceneOpticalMetrics `json:"scene_optical,omitempty"`
	Detections         []FoodDetection      `json:"detections,omitempty"`
	InspectionFoods    []InspectionFood     `json:"inspection_foods,omitempty"`
}

// ToMockInspectionRecord converts an InspectionRecord originating from a mock video source into a MockInspectionRecord.
func (r InspectionRecord) ToMockInspectionRecord(videoSource string) MockInspectionRecord {
	if videoSource == "" && r.ImageS3URI != nil && *r.ImageS3URI != "" {
		videoSource = *r.ImageS3URI
	}
	mockID := r.ID
	if mockID == "" {
		mockID = r.DeviceID
	}
	if mockID == "" {
		mockID = "MOCK_VIDEO_CAM"
	}
	return MockInspectionRecord{
		ID:                 r.ID,
		MockID:             mockID,
		VideoUUID:          r.ID,
		DeviceID:           r.DeviceID,
		SourceType:         "mock_video",
		InspectionStruct:   "MockInspectionRecord",
		VideoSource:        videoSource,
		ImageS3URI:         r.ImageS3URI,
		EnginePipeline:     "YOLOv8n-INT8 + OpenCV HSV",
		CreatedAt:          r.CreatedAt,
		IsSpoiled:          r.IsSpoiled,
		TSafeHours:         r.TSafeHours,
		LuxLuminance:       r.LuxLuminance,
		WhiteBalanceStatus: r.WhiteBalanceStatus,
		ColorTemperature:   r.ColorTemperature,
		InspectionFoods:    r.InspectionFoods,
	}
}
