package api

import (
	"bawal_vision_backend/internal/device"
	"bawal_vision_backend/internal/inspection"
	"log"
	"time"

	fiber "github.com/gofiber/fiber/v2"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

func RegisterDeviceRoutes(router fiber.Router, db *gorm.DB) {
	router.Post("/telemetry", func(c *fiber.Ctx) error {
		var payload inspection.TelemetryIngressPayload
		log.Printf("[RAW PAYLOAD]: %s", string(c.Body()))

		if err := c.BodyParser(&payload); err != nil {
			return c.Status(400).JSON(fiber.Map{
				"error": "Invalid JSON payload",
			})
		}

		if payload.DeviceID == "" {
			return c.Status(400).JSON(fiber.Map{
				"error": "Missing device_id",
			})
		}

		now := time.Now()

		// Auto-provision or update device telemetry heartbeat & status
		dev := device.Device{
			ID:              payload.DeviceID,
			DeviceType:      "ESP32-S3",
			DeviceCore:      payload.DeviceCore,
			FirmwareVersion: payload.FirmwareVersion,
			Status:          "ACTIVE",
			LastSeenAt:      &now,
			UpdatedAt:       now,
		}

		if err := db.Clauses(clause.OnConflict{
			Columns:   []clause.Column{{Name: "id"}},
			DoUpdates: clause.AssignmentColumns([]string{"firmware_version", "device_core", "last_seen_at", "updated_at", "status"}),
		}).Create(&dev).Error; err != nil {
			log.Printf("[ERROR] Failed to upsert device: %v", err)
			return c.Status(500).JSON(fiber.Map{
				"error": "Failed to register or update device state",
			})
		}

		record := inspection.InspectionRecord{
			ID:              uuid.New().String(),
			DeviceID:        payload.DeviceID,
			FirmwareVersion: payload.FirmwareVersion,
			CreatedAt:       now,
			Temperature:     payload.Temperature,
			Humidity:        payload.Humidity,
			GasVOC:          payload.GasVOC,
			Pressure:        payload.Pressure,
			CPULoad:         payload.CurrentCPULoad,
			IsSpoiled:       false,
			TSafeHours:      0.0,
		}

		if err := db.Create(&record).Error; err != nil {
			log.Printf("[ERROR] Failed to insert inspection record: %v", err)
			return c.Status(500).JSON(fiber.Map{
				"error": "Failed to save data to database",
			})
		}

		return c.Status(201).JSON(fiber.Map{
			"status": "success",
			"id":     record.ID,
		})
	})
}
