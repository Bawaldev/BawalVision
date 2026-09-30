package api

import (
	"bawal_vision_backend/internal/device"
	"bawal_vision_backend/internal/food"
	"bawal_vision_backend/internal/inspection"
	"errors"
	"log"
	"strings"
	"time"

	fiber "github.com/gofiber/fiber/v2"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

func RegisterDeviceRoutes(router fiber.Router, db *gorm.DB) {
	// 1. ESP32-S3 Environmental Sensor Telemetry Ingress
	router.Post("/telemetry", func(c *fiber.Ctx) error {
		var payload inspection.TelemetryIngressPayload
		log.Printf("[TELEMETRY INGRESS]: %s", string(c.Body()))

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

		// Cache in-memory for immediate fusion with vision pipeline detections
		inspection.GlobalInspectionStore.StoreTelemetry(payload)

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

	// 2. Vision + Telemetry Combined Inspection Ingress Handler
	inspectionIngressHandler := func(c *fiber.Ctx) error {
		var req inspection.IngressInspectionRequest
		log.Printf("[INSPECTION INGRESS]: %s", string(c.Body()))

		if err := c.BodyParser(&req); err != nil {
			return c.Status(400).JSON(fiber.Map{
				"error": "Invalid JSON payload",
			})
		}

		if req.DeviceID == "" {
			return c.Status(400).JSON(fiber.Map{
				"error": "Missing device_id in inspection request",
			})
		}

		if len(req.Detections) == 0 {
			return c.Status(400).JSON(fiber.Map{
				"error": "Detections list cannot be empty",
			})
		}

		// Retrieve latest telemetry cache for this device as fallback for missing sensor values
		latestTelemetry, hasCached := inspection.GlobalInspectionStore.GetLatestTelemetry(req.DeviceID)

		temp := 25.0
		if req.Temperature != nil {
			temp = *req.Temperature
		} else if hasCached && latestTelemetry.Temperature != 0 {
			temp = latestTelemetry.Temperature
		}

		cpu := uint8(10)
		if req.CPULoad != nil {
			cpu = *req.CPULoad
		} else if hasCached {
			cpu = latestTelemetry.CurrentCPULoad
		}

		fwVer := "1.0.0"
		if req.FirmwareVersion != "" {
			fwVer = req.FirmwareVersion
		} else if hasCached && latestTelemetry.FirmwareVersion != "" {
			fwVer = latestTelemetry.FirmwareVersion
		}

		now := time.Now()
		recordID := uuid.New().String()

		err := db.Transaction(func(tx *gorm.DB) error {
			// Auto-provision device if not exists
			dev := device.Device{
				ID:              req.DeviceID,
				DeviceType:      "ESP32-S3",
				FirmwareVersion: fwVer,
				Status:          "ACTIVE",
				LastSeenAt:      &now,
				UpdatedAt:       now,
			}
			if err := tx.Clauses(clause.OnConflict{
				Columns:   []clause.Column{{Name: "id"}},
				DoUpdates: clause.AssignmentColumns([]string{"firmware_version", "last_seen_at", "updated_at", "status"}),
			}).Create(&dev).Error; err != nil {
				return err
			}

			// Insert Inspection Record
			record := inspection.InspectionRecord{
				ID:              recordID,
				UserID:          req.UserID,
				DeviceID:        req.DeviceID,
				FirmwareVersion: fwVer,
				CreatedAt:       now,
				ImageS3URI:      req.ImageS3URI,
				Temperature:     temp,
				CPULoad:         cpu,
				IsSpoiled:       false,
				TSafeHours:      0.0,
			}
			if err := tx.Create(&record).Error; err != nil {
				return err
			}

			// Insert detected food items into food_items and inspection_foods
			insertedFoodMap := make(map[int]bool)
			for _, det := range req.Detections {
				foodName := strings.TrimSpace(strings.ToLower(det.FoodName))
				if foodName == "" || foodName == "no_food" || foodName == "no_model" {
					continue
				}

				var foodItem food.FoodItem
				if err := tx.Where("LOWER(name) = ?", foodName).First(&foodItem).Error; err != nil {
					if errors.Is(err, gorm.ErrRecordNotFound) {
						foodItem = food.FoodItem{Name: foodName}
						if err := tx.Create(&foodItem).Error; err != nil {
							// In case of unique constraint race
							if err := tx.Where("LOWER(name) = ?", foodName).First(&foodItem).Error; err != nil {
								return err
							}
						}
					} else {
						return err
					}
				}

				// Avoid duplicate food item insertions for the same inspection ID (constraint uq_inspection_food)
				if insertedFoodMap[foodItem.ID] {
					continue
				}
				insertedFoodMap[foodItem.ID] = true

				weight := det.WeightGrams
				if weight <= 0 {
					weight = 100.0 // Baseline nominal weight if depth estimation unavailable
				}

				inspFood := inspection.InspectionFood{
					ID:           uuid.New().String(),
					InspectionID: recordID,
					FoodItemID:   foodItem.ID,
					WeightGrams:  weight,
				}
				if err := tx.Create(&inspFood).Error; err != nil {
					return err
				}
			}

			return nil
		})

		if err != nil {
			log.Printf("[ERROR] Failed to save inspection transaction: %v", err)
			return c.Status(500).JSON(fiber.Map{
				"error": "Failed to save inspection and detected food records to database",
			})
		}

		return c.Status(201).JSON(fiber.Map{
			"status":               "success",
			"id":                   recordID,
			"device_id":            req.DeviceID,
			"detected_foods_count": len(req.Detections),
		})
	}

	router.Post("/inspections", inspectionIngressHandler)
	router.Post("/inspections/ingress", inspectionIngressHandler)
	router.Post("/inspection/ingress", inspectionIngressHandler)

	// 3. GET Latest Environmental Telemetry
	router.Get("/telemetry/latest", func(c *fiber.Ctx) error {
		var record inspection.InspectionRecord
		if err := db.Order("created_at DESC").First(&record).Error; err != nil {
			return c.Status(404).JSON(fiber.Map{"error": "No telemetry data recorded"})
		}
		return c.JSON(fiber.Map{
			"device_id":        record.DeviceID,
			"firmware_version": record.FirmwareVersion,
			"temperature":      record.Temperature,
			"cpu_load":         record.CPULoad,
			"is_spoiled":       record.IsSpoiled,
			"t_safe_hours":     record.TSafeHours,
			"created_at":       record.CreatedAt,
		})
	})

	// 4. GET Latest Inspection with Detections
	router.Get("/inspections/latest", func(c *fiber.Ctx) error {
		var record inspection.InspectionRecord
		if err := db.Preload("InspectionFoods.FoodItem").Order("created_at DESC").First(&record).Error; err != nil {
			return c.Status(404).JSON(fiber.Map{"error": "No inspection records found"})
		}
		return c.JSON(record)
	})
}
