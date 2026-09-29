package api

import (
	"net/http"
	"strconv"

	"bawal_vision_backend/internal/camera_handler"

	fiber "github.com/gofiber/fiber/v2"
	"gorm.io/gorm"
)

func RegisterCameraRoutes(router fiber.Router, db *gorm.DB) {
	camGroup := router.Group("/camera")

	ingressHandler := func(c *fiber.Ctx) error {
		deviceID := c.Get("X-Device-ID")
		if deviceID == "" {
			deviceID = "DEFAULT_CAMERA"
		}

		frameIDHeader := c.Get("X-Frame-ID", "0")
		frameID, _ := strconv.ParseUint(frameIDHeader, 10, 32)
		body := c.Body()
		if len(body) == 0 {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{"error": "Empty frame body"})
		}

		imageBytes := make([]byte, len(body))
		copy(imageBytes, body)

		camera_handler.GlobalCameraStore.StoreRawFrame(deviceID, uint32(frameID), imageBytes)

		return c.SendStatus(fiber.StatusOK)
	}

	camGroup.Post("/stream", ingressHandler)
	camGroup.Post("/frame", ingressHandler)

	camGroup.Get("/active", func(c *fiber.Ctx) error {
		activeID := camera_handler.GlobalCameraStore.GetActiveDeviceID()
		if activeID == "" {
			return c.Status(fiber.StatusNotFound).JSON(fiber.Map{"error": "No active camera device found"})
		}
		return c.JSON(fiber.Map{"device_id": activeID})
	})

	rawFrameHandler := func(c *fiber.Ctx) error {
		deviceID := c.Params("device_id")
		if deviceID == "" || deviceID == "latest" || deviceID == "raw" || deviceID == "UNKNOWN_DEVICE" || deviceID == "*" {
			deviceID = ""
		}

		rawBytes, exists := camera_handler.GlobalCameraStore.GetRawFrame(deviceID)
		if !exists || len(rawBytes) == 0 {
			return c.Status(fiber.StatusNotFound).JSON(fiber.Map{
				"error": "No raw image available for device",
			})
		}

		c.Set("Content-Type", "image/jpeg")
		c.Set("Cache-Control", "no-cache, no-store, must-revalidate")
		return c.Status(http.StatusOK).Send(rawBytes)
	}

	camGroup.Get("/latest/raw", rawFrameHandler)
	camGroup.Get("/latest/:device_id/raw", rawFrameHandler)
	camGroup.Get("/latest", rawFrameHandler)
	camGroup.Get("/raw", rawFrameHandler)
}
