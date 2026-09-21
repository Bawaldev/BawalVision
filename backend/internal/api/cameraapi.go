package api

import (
	"encoding/base64"
	"net/http"
	"time"

	"bawal_vision_backend/internal/camera_handler"
	"bawal_vision_backend/internal/device"

	fiber "github.com/gofiber/fiber/v2"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

// RegisterCameraRoutes registers all camera ingestion and streaming endpoints
func RegisterCameraRoutes(router fiber.Router, db *gorm.DB) {
	camGroup := router.Group("/camera")

	// 1. Ingress Endpoint: Catches frames streamed from ESP-IDF camera_task
	camGroup.Post("/frame", func(c *fiber.Ctx) error {
		var payload camera_handler.FrameIngressPayload
		if err := c.BodyParser(&payload); err != nil {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{
				"error": "Invalid JSON frame payload",
			})
		}

		if payload.DeviceID == "" {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{
				"error": "Missing device_id",
			})
		}

		if payload.TimestampMS == 0 {
			payload.TimestampMS = time.Now().UnixMilli()
		}

		// Store in thread-safe in-memory cache for ultra-fast OpenCV query
		camera_handler.GlobalCameraStore.StoreLatestFrame(payload)

		now := time.Now()

		// Auto-provision or update device heartbeat
		dev := device.Device{
			ID:         payload.DeviceID,
			DeviceType: "ESP32-S3-CAM",
			Status:     "ACTIVE",
			LastSeenAt: &now,
			UpdatedAt:  now,
		}
		_ = db.Clauses(clause.OnConflict{
			Columns:   []clause.Column{{Name: "id"}},
			DoUpdates: clause.AssignmentColumns([]string{"last_seen_at", "updated_at", "status"}),
		}).Create(&dev)

		// Decode base64 bytes for database persistence
		var rawBytes []byte
		if payload.ImageBase64 != "" {
			rawBytes, _ = base64.StdEncoding.DecodeString(payload.ImageBase64)
		}

		frameRecord := camera_handler.CameraFrame{
			ID:          uuid.New().String(),
			DeviceID:    payload.DeviceID,
			FrameID:     payload.FrameID,
			Width:       payload.Width,
			Height:      payload.Height,
			Format:      payload.Format,
			BufferLen:   payload.BufferLen,
			TimestampMS: payload.TimestampMS,
			ImageBytes:  rawBytes,
			CreatedAt:   now,
		}

		if err := db.Create(&frameRecord).Error; err != nil {
			// Even if DB insert fails, memory cache succeeded
			return c.Status(fiber.StatusInternalServerError).JSON(fiber.Map{
				"error": "Failed to persist camera frame to database",
			})
		}

		return c.Status(fiber.StatusCreated).JSON(fiber.Map{
			"status":   "success",
			"id":       frameRecord.ID,
			"frame_id": frameRecord.FrameID,
		})
	})

	// 2. Fetch Latest Frame Metadata + Base64 (JSON format)
	camGroup.Get("/latest/:device_id", func(c *fiber.Ctx) error {
		deviceID := c.Params("device_id")
		frame, exists := camera_handler.GlobalCameraStore.GetLatestFrame(deviceID)
		if !exists {
			return c.Status(fiber.StatusNotFound).JSON(fiber.Map{
				"error": "No frame found for specified device",
			})
		}
		return c.JSON(frame)
	})

	// 3. Direct Binary Image Endpoint for OpenCV / Web Browser (image/jpeg)
	camGroup.Get("/latest/:device_id/raw", func(c *fiber.Ctx) error {
		deviceID := c.Params("device_id")
		rawBytes, contentType, exists := camera_handler.GlobalCameraStore.GetLatestRawImage(deviceID)
		if !exists || len(rawBytes) == 0 {
			return c.Status(fiber.StatusNotFound).JSON(fiber.Map{
				"error": "No raw image buffer available for specified device",
			})
		}

		c.Set("Content-Type", contentType)
		c.Set("Cache-Control", "no-cache, no-store, must-revalidate")
		return c.Status(http.StatusOK).Send(rawBytes)
	})

	// 4. Query Historical Camera Snapshots from Database
	camGroup.Get("/frames", func(c *fiber.Ctx) error {
		var frames []camera_handler.CameraFrame
		err := db.Order("created_at DESC").Limit(50).Find(&frames).Error
		if err != nil {
			return c.Status(fiber.StatusInternalServerError).JSON(fiber.Map{
				"error": "Failed to query camera frames",
			})
		}
		return c.JSON(frames)
	})
}
