package api

import (
	"bufio"
	"encoding/base64"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"bawal_vision_backend/internal/camera_handler"
	"bawal_vision_backend/internal/inspection"

	fiber "github.com/gofiber/fiber/v2"
	"gorm.io/gorm"
)

func syncTelemetryFromCameraIngress(c *fiber.Ctx, deviceID string, frameID uint32) {
	if deviceID == "" || deviceID == "DEFAULT_CAMERA" || deviceID == "UNKNOWN_DEVICE" {
		return
	}
	// Rewind video does NOT add more telemetry or flood session history
	isRewind := c.Get("X-Is-Rewind") == "1" || c.Get("X-Video-Rewind") == "1"
	if isRewind {
		return
	}

	nowUnix := time.Now().Unix()
	clientIP := c.IP()

	existing, hasExisting := inspection.GlobalInspectionStore.GetLatestTelemetry(deviceID)
	if hasExisting && c.Get("X-ESP-Temp") == "" {
		existing.Timestamp = nowUnix
		if clientIP != "" {
			existing.IPAddress = clientIP
		}
		inspection.GlobalInspectionStore.StoreTelemetry(existing)
		return
	}

	jitter := float64(int(frameID%7)-3) * 0.1
	realTemp := 38.4 + jitter
	cpuTemp := realTemp
	cpuLoad := uint8(22 + (frameID % 9))
	fwVer := "1.0.0"

	if hasExisting {
		if existing.Temperature > 0 {
			realTemp = existing.Temperature
		}
		if existing.CPUTemperature > 0 {
			cpuTemp = existing.CPUTemperature
		}
		if existing.CurrentCPULoad > 0 {
			cpuLoad = existing.CurrentCPULoad
		}
		if existing.FirmwareVersion != "" {
			fwVer = existing.FirmwareVersion
		}
	}

	if hdrTemp := c.Get("X-ESP-Temp"); hdrTemp != "" {
		if v, err := strconv.ParseFloat(hdrTemp, 64); err == nil && v > 0 {
			realTemp = v
			cpuTemp = v
		}
	}
	if hdrCpuTemp := c.Get("X-ESP-CPU-Temp"); hdrCpuTemp != "" {
		if v, err := strconv.ParseFloat(hdrCpuTemp, 64); err == nil && v > 0 {
			cpuTemp = v
		}
	}
	if hdrLoad := c.Get("X-ESP-CPU-Load"); hdrLoad != "" {
		if v, err := strconv.ParseUint(hdrLoad, 10, 8); err == nil {
			cpuLoad = uint8(v)
		}
	}
	if hdrFw := c.Get("X-Firmware-Version"); hdrFw != "" {
		fwVer = strings.Clone(hdrFw)
	}

	camModel := "OV5640"
	if hasExisting && existing.CameraModel != "" {
		camModel = existing.CameraModel
	}
	if hdrModel := c.Get("X-Camera-Model"); hdrModel != "" {
		camModel = strings.Clone(hdrModel)
	}

	connMode := "wifi_lan"
	if deviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(deviceID, "MOCK_") {
		connMode = "mock_video"
	}

	inspection.GlobalInspectionStore.StoreTelemetry(inspection.TelemetryIngressPayload{
		DeviceID:        deviceID,
		FirmwareVersion: fwVer,
		DeviceCore:      2,
		Timestamp:       nowUnix,
		Temperature:     realTemp,
		CPUTemperature:  cpuTemp,
		CurrentCPULoad:  cpuLoad,
		IPAddress:       clientIP,
		ConnectionMode:  connMode,
		CameraModel:     camModel,
	})
}

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
					devID := strings.Clone(payload.DeviceID)
					ipStr := strings.Clone(c.IP())
					isRewind := c.Get("X-Is-Rewind") == "1" || c.Get("X-Video-Rewind") == "1"
					camera_handler.GlobalCameraStore.StoreRawFrameWithMeta(devID, payload.FrameID, imageBytes, ipStr, isRewind)
					syncTelemetryFromCameraIngress(c, devID, payload.FrameID)
				}
				return c.Status(fiber.StatusCreated).JSON(fiber.Map{
					"status":    "success",
					"device_id": payload.DeviceID,
					"frame_id":  payload.FrameID,
				})
			}
		}

		deviceID := strings.Clone(c.Get("X-Device-ID"))
		if deviceID == "" {
			deviceID = "DEFAULT_CAMERA"
		}

		frameIDHeader := c.Get("X-Frame-ID", "0")
		frameID, _ := strconv.ParseUint(frameIDHeader, 10, 32)

		imageBytes := make([]byte, len(body))
		copy(imageBytes, body)

		ipStr := strings.Clone(c.IP())
		isRewind := c.Get("X-Is-Rewind") == "1" || c.Get("X-Video-Rewind") == "1"
		camera_handler.GlobalCameraStore.StoreRawFrameWithMeta(deviceID, uint32(frameID), imageBytes, ipStr, isRewind)
		syncTelemetryFromCameraIngress(c, deviceID, uint32(frameID))

		return c.SendStatus(fiber.StatusOK)
	}

	annotatedIngressHandler := func(c *fiber.Ctx) error {
		body := c.Body()
		if len(body) == 0 {
			return c.SendStatus(fiber.StatusBadRequest)
		}

		deviceID := strings.Clone(c.Get("X-Device-ID"))
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

	videoIngressHandler := func(c *fiber.Ctx) error {
		body := c.Body()
		if len(body) == 0 {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{"error": "Empty video payload"})
		}
		deviceID := c.Get("X-Device-ID", "DEFAULT_DEVICE")
		contentType := c.Get("Content-Type", "video/mp4")
		return c.Status(fiber.StatusCreated).JSON(fiber.Map{
			"status":       "success",
			"device_id":    deviceID,
			"content_type": contentType,
			"bytes":        len(body),
		})
	}
	router.Post("/video", videoIngressHandler)
	camGroup.Post("/video", videoIngressHandler)

	camGroup.Get("/active", func(c *fiber.Ctx) error {
		activeID := camera_handler.GlobalCameraStore.GetActiveDeviceID()
		if activeID == "" {
			return c.Status(fiber.StatusNotFound).JSON(fiber.Map{
				"error":     "No active camera device found",
				"active":    false,
				"device_id": "",
				"fps":       0.0,
			})
		}
		fps := camera_handler.GlobalCameraStore.GetActiveFPS()
		camModel := "OV5640"
		if live, ok := inspection.GlobalInspectionStore.GetLatestTelemetry(activeID); ok && live.CameraModel != "" {
			camModel = live.CameraModel
		}
		return c.JSON(fiber.Map{
			"active":       true,
			"device_id":    activeID,
			"fps":          fps,
			"camera_model": camModel,
		})
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

		rawBytes, isRewind, exists := camera_handler.GlobalCameraStore.GetRawFrameWithRewind(deviceID)
		if !exists || len(rawBytes) == 0 {
			return c.Status(fiber.StatusNotFound).JSON(fiber.Map{
				"error": "No raw image available for device",
			})
		}

		if isRewind {
			c.Set("X-Is-Rewind", "1")
			c.Set("X-Video-Rewind", "1")
		}

		c.Set("Content-Type", "image/jpeg")
		c.Set("Cache-Control", "no-cache, no-store, must-revalidate")
		return c.Status(http.StatusOK).Send(rawBytes)
	}

	camGroup.Get("/frame", rawFrameHandler)
	camGroup.Get("/frame/:device_id", rawFrameHandler)
	camGroup.Get("/latest/raw", rawFrameHandler)
	camGroup.Get("/latest/:device_id/raw", rawFrameHandler)
	camGroup.Get("/latest/:device_id", rawFrameHandler)
	camGroup.Get("/:device_id/raw", rawFrameHandler)
	camGroup.Get("/:device_id", rawFrameHandler)
	camGroup.Get("/latest", rawFrameHandler)
	camGroup.Get("/raw", rawFrameHandler)
}
