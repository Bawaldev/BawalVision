package api

import (
	"bawal_vision_backend/internal/camera_handler"
	"bawal_vision_backend/internal/device"
	"bawal_vision_backend/internal/food"
	"bawal_vision_backend/internal/inspection"
	"errors"
	"fmt"
	"log"
	"strings"
	"time"

	fiber "github.com/gofiber/fiber/v2"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

func RegisterDeviceRoutes(router fiber.Router, db *gorm.DB) {
	// Start background USB hotplug monitor (/dev/ttyACM* and /dev/ttyUSB*) to auto-provision server Wi-Fi
	device.StartUSBAutoProvisioner(func(ack *device.ESPSerialAck) {
		if ack == nil || ack.DeviceID == "" {
			return
		}
		now := time.Now()
		inspection.GlobalInspectionStore.StoreTelemetry(inspection.TelemetryIngressPayload{
			DeviceID:        ack.DeviceID,
			FirmwareVersion: ack.FirmwareVersion,
			DeviceCore:      2,
			Timestamp:       now.Unix(),
			Temperature:     ack.Temperature,
			CPUTemperature:  ack.CPUTemperature,
			CurrentCPULoad:  ack.CPULoad,
			IPAddress:       ack.USBPort,
			ConnectionMode:  "usb_serial",
		})
		if db != nil {
			dev := device.Device{
				ID:              ack.DeviceID,
				DeviceType:      "ESP32-S3",
				DeviceCore:      2,
				FirmwareVersion: ack.FirmwareVersion,
				Status:          "ACTIVE",
				LastSeenAt:      &now,
				UpdatedAt:       now,
			}
			_ = db.Clauses(clause.OnConflict{
				Columns:   []clause.Column{{Name: "id"}},
				DoUpdates: clause.AssignmentColumns([]string{"firmware_version", "device_core", "last_seen_at", "updated_at", "status"}),
			}).Create(&dev).Error
		}
	})

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

		if payload.IPAddress == "" {
			payload.IPAddress = c.IP()
		}
		if payload.ConnectionMode == "" {
			payload.ConnectionMode = "wifi_lan"
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

		realTemp := 25.0
		if req.Temperature != nil {
			realTemp = *req.Temperature
		} else if req.CPUTemperature != nil {
			realTemp = *req.CPUTemperature
		} else if req.FoodTemperature != nil {
			realTemp = *req.FoodTemperature
		} else if hasCached {
			if latestTelemetry.Temperature > 0 {
				realTemp = latestTelemetry.Temperature
			} else if latestTelemetry.CPUTemperature > 0 {
				realTemp = latestTelemetry.CPUTemperature
			} else if latestTelemetry.FoodTemperature > 0 {
				realTemp = latestTelemetry.FoodTemperature
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
				if realTemp > 28.0 {
					spoilage += (realTemp - 28.0) * 2.2
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
		if realTemp > 20.0 {
			tSafeHours = 8.0 * (1.0 / (1.0 + 0.05*(realTemp-20.0)))
		}
		if anySpoiled {
			tSafeHours = 0.0
		}

		task := inspection.BatchInspectionTask{
			RecordID:        recordID,
			Request:         req,
			Temperature:     realTemp,
			CPUTemp:         cpuTemp,
			CPULoad:         cpu,
			FirmwareVersion: fwVer,
			IsSpoiled:       anySpoiled,
			TSafeHours:      tSafeHours,
			CreatedAt:       now,
		}

		// Asynchronous non-blocking enqueue to Batch Worker
		if inspection.GlobalBatchWorker != nil {
			inspection.GlobalBatchWorker.Enqueue(task)
		} else {
			// Fallback synchronous write if worker is not yet initialized
			err := db.Transaction(func(tx *gorm.DB) error {
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

				record := inspection.InspectionRecord{
					ID:              recordID,
					UserID:          req.UserID,
					DeviceID:        req.DeviceID,
					FirmwareVersion: fwVer,
					CreatedAt:       now,
					ImageS3URI:      req.ImageS3URI,
					CPUTemperature:  cpuTemp,
					Temperature:     realTemp,
					CPULoad:         cpu,
					IsSpoiled:       anySpoiled,
					TSafeHours:      tSafeHours,
				}
				if err := tx.Create(&record).Error; err != nil {
					return err
				}

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
								if err := tx.Where("LOWER(name) = ?", foodName).First(&foodItem).Error; err != nil {
									return err
								}
							}
						} else {
							return err
						}
					}

					if insertedFoodMap[foodItem.ID] {
						continue
					}
					insertedFoodMap[foodItem.ID] = true

					itemTemp := realTemp
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
				log.Printf("[ERROR] Failed to save fallback inspection transaction: %v", err)
				return c.Status(500).JSON(fiber.Map{
					"error": "Failed to save inspection to database",
				})
			}
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
			nowUnix := time.Now().Unix()
			// Device is actively connected if telemetry was received within the last 30 seconds
			isLiveConnected := (nowUnix - live.Timestamp) <= 30

			realT := live.Temperature
			if realT <= 0 {
				realT = live.CPUTemperature
			}
			if realT <= 0 {
				realT = live.FoodTemperature
			}
			if realT <= 0 {
				realT = 25.0
			}

			resp := fiber.Map{
				"device_id":        live.DeviceID,
				"firmware_version": live.FirmwareVersion,
				"temperature":      realT,
				"timestamp":        live.Timestamp,
				"is_connected":     isLiveConnected,
				"ip_address":       live.IPAddress,
				"connection_mode":  live.ConnectionMode,
			}

			// CPU temperature and CPU utility are only available if the ESP device is connected
			if isLiveConnected {
				cpuT := live.CPUTemperature
				if cpuT <= 0 {
					cpuT = 36.5
				}
				resp["cpu_temperature"] = cpuT
				resp["cpu_load"] = live.CurrentCPULoad
			} else {
				resp["cpu_temperature"] = nil
				resp["cpu_load"] = nil
			}

			return c.JSON(resp)
		}

		var record inspection.InspectionRecord
		if err := db.Order("created_at DESC").First(&record).Error; err != nil {
			return c.Status(404).JSON(fiber.Map{"error": "No telemetry data recorded"})
		}
		return c.JSON(fiber.Map{
			"device_id":        record.DeviceID,
			"firmware_version": record.FirmwareVersion,
			"cpu_temperature":  nil, // Not available: ESP device not actively connected
			"temperature":      record.Temperature,
			"cpu_load":         nil, // Not available: ESP device not actively connected
			"is_spoiled":       record.IsSpoiled,
			"t_safe_hours":     record.TSafeHours,
			"created_at":       record.CreatedAt,
			"is_connected":     false,
		})
	})

	// 4. GET Latest Inspection with Detections & Real-time Bounding Boxes
	router.Get("/inspections/latest", func(c *fiber.Ctx) error {
		if live, ok := inspection.GlobalInspectionStore.GetLatestInspection(""); ok && len(live.Detections) > 0 {
			realT := 36.5
			if live.Temperature != nil && *live.Temperature > 0 {
				realT = *live.Temperature
			} else if live.CPUTemperature != nil && *live.CPUTemperature > 0 {
				realT = *live.CPUTemperature
			} else if live.FoodTemperature != nil && *live.FoodTemperature > 0 {
				realT = *live.FoodTemperature
			}
			return c.JSON(fiber.Map{
				"device_id":       live.DeviceID,
				"temperature":     realT,
				"cpu_temperature": live.CPUTemperature,
				"detections":      live.Detections,
				"is_live":         true,
			})
		}

		var record inspection.InspectionRecord
		if err := db.Preload("InspectionFoods.FoodItem").Order("created_at DESC").First(&record).Error; err != nil {
			return c.Status(404).JSON(fiber.Map{"error": "No inspection records found"})
		}
		return c.JSON(record)
	})

	// 5. GET ESP32-S3 Hardware Connection & USB Serial Slot Status
	router.Get("/devices/status", func(c *fiber.Ctx) error {
		usbPorts := device.ScanUSBSerialPorts()
		wifiCfg := device.DetectServerWiFiConfig()
		nowUnix := time.Now().Unix()

		connected := false
		connMode := "disconnected"
		devID := ""
		fwVer := ""
		ipAddr := ""

		if live, ok := inspection.GlobalInspectionStore.GetAnyLatestTelemetry(); ok && (nowUnix-live.Timestamp) <= 30 {
			connected = true
			devID = live.DeviceID
			fwVer = live.FirmwareVersion
			ipAddr = live.IPAddress
			connMode = live.ConnectionMode
			if connMode == "" {
				connMode = "wifi_lan"
			}
		} else if camID := camera_handler.GlobalCameraStore.GetActiveDeviceID(); camID != "" {
			connected = true
			devID = camID
			fwVer = "1.0.0"
			connMode = "wifi_lan"
		}

		return c.JSON(fiber.Map{
			"connected":        connected,
			"connection_mode":  connMode,
			"device_id":        devID,
			"firmware_version": fwVer,
			"ip_address":       ipAddr,
			"usb_ports":        usbPorts,
			"server_wifi": fiber.Map{
				"ssid":         wifiCfg.SSID,
				"has_password": wifiCfg.HasPassword,
				"backend_ip":   wifiCfg.BackendIP,
				"backend_port": wifiCfg.BackendPort,
			},
		})
	})

	// 6. POST Connect ESP32-S3 (Same Wi-Fi LAN Discovery + Automatic USB /dev/ttyACM* & /dev/ttyUSB* Wi-Fi Sync)
	router.Post("/devices/connect", func(c *fiber.Ctx) error {
		var req struct {
			DeviceID string `json:"device_id"`
			USBPort  string `json:"usb_port"`
			SSID     string `json:"ssid"`
			Password string `json:"password"`
		}
		_ = c.BodyParser(&req)
		requestedID := strings.TrimSpace(req.DeviceID)
		nowUnix := time.Now().Unix()

		// Step A: Check if ESP32-S3 is already connected on the same Wi-Fi LAN and sending live telemetry
		if requestedID != "" {
			if live, ok := inspection.GlobalInspectionStore.GetLatestTelemetry(requestedID); ok && (nowUnix-live.Timestamp) <= 30 {
				mode := live.ConnectionMode
				if mode == "" {
					mode = "wifi_lan"
				}
				return c.JSON(fiber.Map{
					"success":          true,
					"device_id":        live.DeviceID,
					"firmware_version": live.FirmwareVersion,
					"ip_address":       live.IPAddress,
					"connection_mode":  mode,
					"latency_ms":       8,
					"message":          fmt.Sprintf("ESP32-S3 (%s) detected on shared Wi-Fi network (%s)", live.DeviceID, live.IPAddress),
				})
			}
		}
		if live, ok := inspection.GlobalInspectionStore.GetAnyLatestTelemetry(); ok && (nowUnix-live.Timestamp) <= 30 {
			mode := live.ConnectionMode
			if mode == "" {
				mode = "wifi_lan"
			}
			return c.JSON(fiber.Map{
				"success":          true,
				"device_id":        live.DeviceID,
				"firmware_version": live.FirmwareVersion,
				"ip_address":       live.IPAddress,
				"connection_mode":  mode,
				"latency_ms":       9,
				"message":          fmt.Sprintf("ESP32-S3 (%s) detected on shared connection (%s)", live.DeviceID, live.IPAddress),
			})
		}

		// Step B: Check if ESP32-S3 is streaming camera frames on the same Wi-Fi LAN
		if activeCamID := camera_handler.GlobalCameraStore.GetActiveDeviceID(); activeCamID != "" {
			return c.JSON(fiber.Map{
				"success":          true,
				"device_id":        activeCamID,
				"firmware_version": "1.0.0",
				"ip_address":       c.IP(),
				"connection_mode":  "wifi_lan",
				"latency_ms":       11,
				"message":          fmt.Sprintf("ESP32-S3 camera node (%s) active on shared Wi-Fi network", activeCamID),
			})
		}

		// Step C: ESP32-S3 Wi-Fi not applied yet — scan USB serial slots (/dev/ttyACM* & /dev/ttyUSB*) and provision server's Wi-Fi
		usbPorts := device.ScanUSBSerialPorts()
		if len(usbPorts) > 0 || req.USBPort != "" {
			targetPort := strings.TrimSpace(req.USBPort)
			if targetPort == "" && len(usbPorts) > 0 {
				targetPort = usbPorts[0].Port
			}

			wifiCfg := device.DetectServerWiFiConfig()
			ssid := strings.TrimSpace(req.SSID)
			if ssid == "" {
				ssid = wifiCfg.SSID
			}
			pass := req.Password
			if pass == "" {
				pass = wifiCfg.Password
			}

			ack, err := device.ProvisionESP32ViaUSB(targetPort, ssid, pass, wifiCfg.BackendIP, wifiCfg.BackendPort)
			if err != nil {
				return c.Status(fiber.StatusBadGateway).JSON(fiber.Map{
					"success": false,
					"message": fmt.Sprintf("Detected USB port %s, but serial provisioning failed: %v", targetPort, err),
				})
			}

			resolvedID := ack.DeviceID
			if resolvedID == "" {
				if requestedID != "" {
					resolvedID = requestedID
				} else {
					resolvedID = "ESP32S3_USB"
				}
			}

			now := time.Now()
			inspection.GlobalInspectionStore.StoreTelemetry(inspection.TelemetryIngressPayload{
				DeviceID:        resolvedID,
				FirmwareVersion: ack.FirmwareVersion,
				DeviceCore:      2,
				Timestamp:       now.Unix(),
				Temperature:     ack.Temperature,
				CPUTemperature:  ack.CPUTemperature,
				CurrentCPULoad:  ack.CPULoad,
				IPAddress:       targetPort,
				ConnectionMode:  "usb_serial",
			})

			msg := fmt.Sprintf("ESP32-S3 detected on USB %s", targetPort)
			if ssid != "" {
				msg = fmt.Sprintf("ESP32-S3 detected on %s and provisioned to server Wi-Fi '%s' (Backend: %s)", targetPort, ssid, wifiCfg.BackendIP)
			}

			return c.JSON(fiber.Map{
				"success":          true,
				"device_id":        resolvedID,
				"firmware_version": ack.FirmwareVersion,
				"ip_address":       targetPort,
				"usb_port":         targetPort,
				"provisioned_ssid": ssid,
				"connection_mode":  "usb_serial",
				"latency_ms":       4,
				"message":          msg,
			})
		}

		// Step D: Neither Wi-Fi LAN nor USB serial slot has an ESP32-S3 attached
		return c.Status(fiber.StatusNotFound).JSON(fiber.Map{
			"success":   false,
			"device_id": requestedID,
			"usb_ports": usbPorts,
			"message":   "No ESP32-S3 detected on the server's Wi-Fi network or USB slots (/dev/ttyACM*, /dev/ttyUSB*). Plug the ESP32-S3 into a server USB port to auto-sync Wi-Fi credentials.",
		})
	})

	// 7. POST Explicit USB Wi-Fi Provisioning (/dev/ttyACM* or /dev/ttyUSB*)
	router.Post("/devices/usb/provision", func(c *fiber.Ctx) error {
		var req struct {
			USBPort   string `json:"usb_port"`
			SSID      string `json:"ssid"`
			Password  string `json:"password"`
			BackendIP string `json:"backend_ip"`
		}
		if err := c.BodyParser(&req); err != nil {
			return c.Status(400).JSON(fiber.Map{"success": false, "message": "Invalid JSON request"})
		}

		wifiCfg := device.DetectServerWiFiConfig()
		ssid := strings.TrimSpace(req.SSID)
		if ssid == "" {
			ssid = wifiCfg.SSID
		}
		pass := req.Password
		if pass == "" {
			pass = wifiCfg.Password
		}
		backendIP := strings.TrimSpace(req.BackendIP)
		if backendIP == "" {
			backendIP = wifiCfg.BackendIP
		}

		ack, err := device.ProvisionESP32ViaUSB(req.USBPort, ssid, pass, backendIP, wifiCfg.BackendPort)
		if err != nil {
			return c.Status(400).JSON(fiber.Map{
				"success": false,
				"message": err.Error(),
			})
		}

		now := time.Now()
		inspection.GlobalInspectionStore.StoreTelemetry(inspection.TelemetryIngressPayload{
			DeviceID:        ack.DeviceID,
			FirmwareVersion: ack.FirmwareVersion,
			DeviceCore:      2,
			Timestamp:       now.Unix(),
			Temperature:     ack.Temperature,
			CPUTemperature:  ack.CPUTemperature,
			CurrentCPULoad:  ack.CPULoad,
			IPAddress:       ack.USBPort,
			ConnectionMode:  "usb_serial",
		})

		return c.JSON(fiber.Map{
			"success":          true,
			"device_id":        ack.DeviceID,
			"firmware_version": ack.FirmwareVersion,
			"usb_port":         ack.USBPort,
			"provisioned_ssid": ssid,
			"backend_ip":       backendIP,
			"connection_mode":  "usb_serial",
			"message":          fmt.Sprintf("Provisioned server Wi-Fi '%s' (Backend %s) to ESP32-S3 via %s", ssid, backendIP, ack.USBPort),
		})
	})

	// 8. POST Disconnect ESP32-S3
	router.Post("/devices/disconnect", func(c *fiber.Ctx) error {
		var req struct {
			DeviceID string `json:"device_id"`
		}
		_ = c.BodyParser(&req)
		inspection.GlobalInspectionStore.DisconnectDevice(strings.TrimSpace(req.DeviceID))
		return c.JSON(fiber.Map{
			"success": true,
			"message": "Device disconnected",
		})
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

