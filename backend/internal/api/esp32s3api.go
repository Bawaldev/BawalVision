package api

import (
	"bawal_vision_backend/internal/inspection"
	"log"
	"time"

	fiber "github.com/gofiber/fiber/v2"
	"github.com/google/uuid"
	"gorm.io/gorm"
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

		record := inspection.InspectionRecord{
			ID:        uuid.New().String(),
			DeviceID:  payload.DeviceID,
			CreatedAt: time.Now(),
			Telemetry: inspection.TelemetryIngressPayload{
				Temperature:    payload.Temperature,
				Humidity:       payload.Humidity,
				GasVOC:         payload.GasVOC,
				Pressure:       payload.Pressure,
				CurrentCPULoad: payload.CurrentCPULoad,
			},
			Prediction: inspection.PredictionResult{
				IsSpoiled:  false,
				TotalGrams: 0.0,
				TSafeHours: 0.0,
			},
			Foods: []inspection.Food{},
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
