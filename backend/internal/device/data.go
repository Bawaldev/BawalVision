package device

import (
	"bawal_vision_backend/internal/inspection"
	"time"
)

type Device struct {
	ID              string     `json:"id" gorm:"type:varchar(64);primaryKey"`
	DeviceType      string     `json:"device_type" gorm:"column:device_type;type:varchar(50);not null;default:'ESP32'"`
	DeviceCore      uint8      `json:"device_core" gorm:"column:device_core;type:smallint;not null;default:2"`
	FirmwareVersion string     `json:"firmware_version" gorm:"column:firmware_version;type:varchar(32);not null;default:'1.0.0'"`
	KitchenID       *string    `json:"kitchen_id,omitempty" gorm:"column:kitchen_id;type:varchar(64)"`
	Status          string     `json:"status" gorm:"column:status;type:varchar(20);not null;default:'ACTIVE'"`
	LastSeenAt      *time.Time `json:"last_seen_at,omitempty" gorm:"column:last_seen_at;type:timestamptz"`
	CreatedAt       time.Time  `json:"created_at" gorm:"column:created_at;type:timestamptz;not null;default:CURRENT_TIMESTAMP"`
	UpdatedAt       time.Time  `json:"updated_at" gorm:"column:updated_at;type:timestamptz;not null;default:CURRENT_TIMESTAMP"`

	Inspections []inspection.InspectionRecord `json:"inspections,omitempty" gorm:"foreignKey:DeviceID;constraint:OnDelete:RESTRICT"`
}

func (Device) TableName() string {
	return "devices"
}
