package api

import (
	"bufio"
	"encoding/base64"
	"fmt"
	"net/http"
	"strconv"
	"strings"

	"bawal_vision_backend/internal/camera_handler"

	fiber "github.com/gofiber/fiber/v2"
	"gorm.io/gorm"
)

func RegisterCameraRoutes(router fiber.Router, db *gorm.DB) {
	camGroup := router.Group("/camera")

	ingressHandler := func(c *fiber.Ctx) error {
		body := c.Body()
		if len(body) == 0 {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{"error": "Empty frame body"})
		}

		contentType := c.Get("Content-Type")
		isJSON := strings.Contains(contentType, "application/json") || (len(body) > 0 && body[0] == '{')

		if isJSON {
			var payload camera_handler.FrameIngressPayload
			if err := c.BodyParser(&payload); err == nil && payload.DeviceID != "" {
				var imageBytes []byte
				if payload.ImageBase64 != "" {
					decoded, err := base64.StdEncoding.DecodeString(payload.ImageBase64)
					if err == nil {
						imageBytes = decoded
					}
				}
				if len(imageBytes) > 0 {
					camera_handler.GlobalCameraStore.StoreRawFrame(payload.DeviceID, payload.FrameID, imageBytes)
				}
				return c.Status(fiber.StatusCreated).JSON(fiber.Map{
					"status":    "success",
					"device_id": payload.DeviceID,
					"frame_id":  payload.FrameID,
				})
			}
		}

		deviceID := c.Get("X-Device-ID")
		if deviceID == "" {
			deviceID = "DEFAULT_CAMERA"
		}

		frameIDHeader := c.Get("X-Frame-ID", "0")
		frameID, _ := strconv.ParseUint(frameIDHeader, 10, 32)

		imageBytes := make([]byte, len(body))
		copy(imageBytes, body)

		camera_handler.GlobalCameraStore.StoreRawFrame(deviceID, uint32(frameID), imageBytes)

		return c.SendStatus(fiber.StatusOK)
	}

	annotatedIngressHandler := func(c *fiber.Ctx) error {
		body := c.Body()
		if len(body) == 0 {
			return c.SendStatus(fiber.StatusBadRequest)
		}

		deviceID := c.Get("X-Device-ID")
		if deviceID == "" {
			deviceID = "DEFAULT_CAMERA"
		}

		frameIDHeader := c.Get("X-Frame-ID", "0")
		frameID, _ := strconv.ParseUint(frameIDHeader, 10, 32)

		imageBytes := make([]byte, len(body))
		copy(imageBytes, body)

		camera_handler.GlobalCameraStore.StoreAnnotatedFrame(deviceID, uint32(frameID), imageBytes)

		return c.SendStatus(fiber.StatusOK)
	}

	camGroup.Post("/stream", ingressHandler)
	camGroup.Post("/frame", ingressHandler)
	camGroup.Post("/annotated", annotatedIngressHandler)

	camGroup.Get("/active", func(c *fiber.Ctx) error {
		activeID := camera_handler.GlobalCameraStore.GetActiveDeviceID()
		if activeID == "" {
			return c.Status(fiber.StatusNotFound).JSON(fiber.Map{"error": "No active camera device found"})
		}
		return c.JSON(fiber.Map{"device_id": activeID})
	})

	mjpegStreamHandler := func(c *fiber.Ctx) error {
		deviceID := c.Params("device_id")
		if deviceID == "" || deviceID == "latest" || deviceID == "raw" || deviceID == "UNKNOWN_DEVICE" || deviceID == "*" {
			deviceID = ""
		}

		c.Set("Content-Type", "multipart/x-mixed-replace; boundary=frame")
		c.Set("Cache-Control", "no-cache, no-store, must-revalidate, pre-check=0, post-check=0, max-age=0")
		c.Set("Pragma", "no-cache")
		c.Set("Connection", "close")

		c.Context().SetBodyStreamWriter(func(w *bufio.Writer) {
			frameChan, unsubscribe := camera_handler.GlobalCameraStore.Subscribe(deviceID)
			defer unsubscribe()

			// Send current frame immediately if available
			if initialFrame, ok := camera_handler.GlobalCameraStore.GetStreamFrame(deviceID); ok && len(initialFrame) > 0 {
				if _, err := fmt.Fprintf(w, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len(initialFrame)); err == nil {
					_, _ = w.Write(initialFrame)
					_, _ = w.WriteString("\r\n")
					_ = w.Flush()
				}
			}

			for frame := range frameChan {
				if len(frame) == 0 {
					continue
				}
				if _, err := fmt.Fprintf(w, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len(frame)); err != nil {
					break
				}
				if _, err := w.Write(frame); err != nil {
					break
				}
				if _, err := w.WriteString("\r\n"); err != nil {
					break
				}
				if err := w.Flush(); err != nil {
					break
				}
			}
		})

		return nil
	}

	camGroup.Get("/mjpeg", mjpegStreamHandler)
	camGroup.Get("/mjpeg/:device_id", mjpegStreamHandler)
	camGroup.Get("/stream", mjpegStreamHandler)
	camGroup.Get("/stream/:device_id", mjpegStreamHandler)

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
	camGroup.Get("/latest/:device_id", rawFrameHandler)
	camGroup.Get("/:device_id/raw", rawFrameHandler)
	camGroup.Get("/:device_id", rawFrameHandler)
	camGroup.Get("/latest", rawFrameHandler)
	camGroup.Get("/raw", rawFrameHandler)
}
