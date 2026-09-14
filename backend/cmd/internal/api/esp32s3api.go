package api

import (
	"bawal_vision_backend/cmd/internal/inspection"
	"time"

	fiber "github.com/gofiber/fiber/v2"
	"github.com/google/uuid"
	"gorm.io/gorm"
)

func RegisterDeviceRoutes(router fiber.Router, db *gorm.DB) {
	// ESP32 -> Server Ingestion
	router.Post("/ingest", func(c *fiber.Ctx) error {
		var payload inspection.TelemetryIngressPayload

		// Parse the JSON body from ESP32
		if err := c.BodyParser(&payload); err != nil {
			return c.Status(400).JSON(fiber.Map{
				"error": "Invalid JSON payload",
			})
		}

		record := inspection.InspectionRecord{
			ID:        uuid.New().String(),
			DeviceID:  payload.DeviceID,
			CreatedAt: time.Now(),
			Telemetry: payload,
		}

		if err := db.Create(&record).Error; err != nil {
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
