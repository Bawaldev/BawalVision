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

		return c.Status(200).JSON(fiber.Map{
			"status":    "success",
			"device_id": payload.DeviceID,
			"message":   "Telemetry heartbeat recorded",
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

		// Cache live detections in-memory for instant frontend HUD overlay rendering
		inspection.GlobalInspectionStore.StoreLatestInspection(req)

		// Retrieve latest telemetry cache for this device as fallback for missing sensor values
		latestTelemetry, hasCached := inspection.GlobalInspectionStore.GetLatestTelemetry(req.DeviceID)

		foodTemp := 25.0
		if req.FoodTemperature != nil {
			foodTemp = *req.FoodTemperature
		} else if req.Temperature != nil {
			foodTemp = *req.Temperature
		} else if hasCached {
			if latestTelemetry.FoodTemperature > 0 {
				foodTemp = latestTelemetry.FoodTemperature
			} else if latestTelemetry.Temperature > 0 {
				foodTemp = latestTelemetry.Temperature
			}
		}

		cpuTemp := 36.5
		if req.CPUTemperature != nil {
			cpuTemp = *req.CPUTemperature
		} else if hasCached && latestTelemetry.CPUTemperature > 0 {
			cpuTemp = latestTelemetry.CPUTemperature
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

		// CV + ML Quality & Spoilage Evaluation
		anySpoiled := false
		maxSpoilageScore := 0.0

		for _, det := range req.Detections {
			conf := det.SimilarityRate
			if conf <= 0 {
				conf = 0.85
			}
			spoilage := det.SpoilageScore
			if spoilage <= 0 {
				// Food science thermal & confidence decay model
				spoilage = (1.0 - conf) * 30.0
				if foodTemp > 28.0 {
					spoilage += (foodTemp - 28.0) * 2.2
				}
				if spoilage > 100.0 {
					spoilage = 100.0
				}
			}
			if spoilage > maxSpoilageScore {
				maxSpoilageScore = spoilage
			}
			if det.Status == "REJECT" || spoilage >= 55.0 || conf < 0.35 {
				anySpoiled = true
			}
		}

		// Calculate remaining safe shelf life based on thermal index
		tSafeHours := 8.0
		if foodTemp > 20.0 {
			tSafeHours = 8.0 * (1.0 / (1.0 + 0.05*(foodTemp-20.0)))
		}
		if anySpoiled {
			tSafeHours = 0.0
		}

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
				FoodTemperature: foodTemp,
				CPUTemperature:  cpuTemp,
				Temperature:     foodTemp,
				CPULoad:         cpu,
				IsSpoiled:       anySpoiled,
				TSafeHours:      tSafeHours,
			}
			if err := tx.Create(&record).Error; err != nil {
				return err
			}

			// Insert detected food items into food_items and inspection_foods with CV+ML quality analytics
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

				itemTemp := foodTemp
				if det.Temperature > 0 {
					itemTemp = det.Temperature
				}

				conf, spoilage, evaluatedTemp, itemStatus := EvaluateFoodQuality(
					foodName,
					det.SimilarityRate,
					det.SpoilageScore,
					itemTemp,
					det.Status,
				)

				inspFood := inspection.InspectionFood{
					ID:             uuid.New().String(),
					InspectionID:   recordID,
					FoodItemID:     foodItem.ID,
					Temperature:    evaluatedTemp,
					SimilarityRate: conf,
					SpoilageScore:  spoilage,
					Status:         itemStatus,
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
			"is_spoiled":           anySpoiled,
			"t_safe_hours":         tSafeHours,
			"detected_foods_count": len(req.Detections),
		})
	}

	router.Post("/inspections", inspectionIngressHandler)
	router.Post("/inspections/ingress", inspectionIngressHandler)
	router.Post("/inspection/ingress", inspectionIngressHandler)

	// 3. GET Latest Environmental Telemetry
	router.Get("/telemetry/latest", func(c *fiber.Ctx) error {
		if live, ok := inspection.GlobalInspectionStore.GetAnyLatestTelemetry(); ok {
			foodT := live.FoodTemperature
			if foodT <= 0 {
				foodT = live.Temperature
			}
			if foodT <= 0 {
				foodT = 24.5
			}
			cpuT := live.CPUTemperature
			if cpuT <= 0 {
				cpuT = 36.5
			}
			return c.JSON(fiber.Map{
				"device_id":        live.DeviceID,
				"firmware_version": live.FirmwareVersion,
				"food_temperature": foodT,
				"cpu_temperature":  cpuT,
				"temperature":      foodT,
				"cpu_load":         live.CurrentCPULoad,
				"timestamp":        live.Timestamp,
			})
		}

		var record inspection.InspectionRecord
		if err := db.Order("created_at DESC").First(&record).Error; err != nil {
			return c.Status(404).JSON(fiber.Map{"error": "No telemetry data recorded"})
		}
		return c.JSON(fiber.Map{
			"device_id":        record.DeviceID,
			"firmware_version": record.FirmwareVersion,
			"food_temperature": record.FoodTemperature,
			"cpu_temperature":  record.CPUTemperature,
			"temperature":      record.Temperature,
			"cpu_load":         record.CPULoad,
			"is_spoiled":       record.IsSpoiled,
			"t_safe_hours":     record.TSafeHours,
			"created_at":       record.CreatedAt,
		})
	})

	// 4. GET Latest Inspection with Detections & Real-time Bounding Boxes
	router.Get("/inspections/latest", func(c *fiber.Ctx) error {
		if live, ok := inspection.GlobalInspectionStore.GetLatestInspection(""); ok && len(live.Detections) > 0 {
			return c.JSON(fiber.Map{
				"device_id":        live.DeviceID,
				"food_temperature": live.FoodTemperature,
				"temperature":      live.Temperature,
				"cpu_temperature":  live.CPUTemperature,
				"detections":       live.Detections,
				"is_live":          true,
			})
		}

		var record inspection.InspectionRecord
		if err := db.Preload("InspectionFoods.FoodItem").Order("created_at DESC").First(&record).Error; err != nil {
			return c.Status(404).JSON(fiber.Map{"error": "No inspection records found"})
		}
		return c.JSON(record)
	})
}

// EvaluateFoodQuality applies multi-sensor quality & food safety rules combining:
// 1. Computer Vision: Classification similarity & object integrity (Hue/color degradation detection)
// 2. Machine Learning: Visual decomposition & spoilage scoring (ONNX model inference)
// 3. Food Temperature Sensor: Food surface temperature (danger zone > 28°C acceleration)
func EvaluateFoodQuality(foodName string, cvConf float64, mlSpoilage float64, foodTemp float64, requestedStatus string) (float64, float64, float64, string) {
	if foodTemp <= 0 {
		foodTemp = 25.0
	}
	if cvConf <= 0 {
		cvConf = 0.88
	}

	// Dynamic ML Spoilage Model from CV features + Thermal decay
	if mlSpoilage <= 0 {
		mlSpoilage = (1.0 - cvConf) * 25.0
		if foodTemp > 28.0 {
			mlSpoilage += (foodTemp - 28.0) * 2.2
		}
		if mlSpoilage > 100.0 {
			mlSpoilage = 100.0
		}
	}

	status := "PASS"
	if requestedStatus != "" {
		status = requestedStatus
	} else if cvConf < 0.35 || mlSpoilage >= 55.0 || (foodTemp > 42.0 && mlSpoilage >= 40.0) {
		status = "REJECT"
	} else if cvConf < 0.60 || mlSpoilage >= 35.0 || foodTemp > 38.0 {
		status = "REVIEW"
	}

	return cvConf, mlSpoilage, foodTemp, status
}

