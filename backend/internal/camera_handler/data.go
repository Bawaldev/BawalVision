package camera_handler

import (
	"time"

	"gorm.io/gorm"
)

// FrameFormat enum matching ESP-IDF frame_format_t
type FrameFormat uint32

const (
	FormatJPEG      FrameFormat = 0
	FormatRGB565    FrameFormat = 1
	FormatRGB888    FrameFormat = 2
	FormatGrayscale FrameFormat = 3
	FormatRaw       FrameFormat = 4
)

// CameraFrame represents the persisted camera snapshot record in PostgreSQL
type CameraFrame struct {
	ID          string         `json:"id" gorm:"type:uuid;primaryKey;default:gen_random_uuid()"`
	DeviceID    string         `json:"device_id" gorm:"column:device_id;type:varchar(64);not null;index"`
	FrameID     uint32         `json:"frame_id" gorm:"column:frame_id;not null"`
	Width       uint32         `json:"width" gorm:"column:width;not null;default:320"`
	Height      uint32         `json:"height" gorm:"column:height;not null;default:240"`
	Format      uint32         `json:"format" gorm:"column:format;not null;default:0"`
	BufferLen   uint32         `json:"buffer_len" gorm:"column:buffer_len;not null"`
	TimestampMS int64          `json:"timestamp_ms" gorm:"column:timestamp_ms;not null"`
	ImageBytes  []byte         `json:"-" gorm:"column:image_bytes;type:bytea"`
	ImageS3URI  *string        `json:"image_s3_uri,omitempty" gorm:"column:image_s3_uri;type:text"`
	CreatedAt   time.Time      `json:"created_at" gorm:"column:created_at;type:timestamptz;not null;default:CURRENT_TIMESTAMP"`
	DeletedAt   gorm.DeletedAt `json:"-" gorm:"index"`
}

func (CameraFrame) TableName() string {
	return "camera_frames"
}

// FrameIngressPayload represents incoming camera frame data from the ESP-IDF edge node
type FrameIngressPayload struct {
	DeviceID    string `json:"device_id"`
	FrameID     uint32 `json:"frame_id"`
	Width       uint32 `json:"width"`
	Height      uint32 `json:"height"`
	Format      uint32 `json:"format"`
	BufferLen   uint32 `json:"buffer_len"`
	TimestampMS int64  `json:"timestamp_ms"`
	ImageBase64 string `json:"image_base64,omitempty"`
}

// FrameQueryResponse represents the client/OpenCV-facing metadata format
type FrameQueryResponse struct {
	ID          string    `json:"id"`
	DeviceID    string    `json:"device_id"`
	FrameID     uint32    `json:"frame_id"`
	Width       uint32    `json:"width"`
	Height      uint32    `json:"height"`
	Format      string    `json:"format"`
	BufferLen   uint32    `json:"buffer_len"`
	TimestampMS int64     `json:"timestamp_ms"`
	ImageBase64 string    `json:"image_base64,omitempty"`
	CreatedAt   time.Time `json:"created_at"`
}
