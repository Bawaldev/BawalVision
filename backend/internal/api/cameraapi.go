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

func isMockDeviceOrHeader(c *fiber.Ctx, deviceID string) bool {
	if deviceID == "MOCK_VIDEO_CAM" || strings.HasPrefix(deviceID, "MOCK_") {
		return true
	}
	if c != nil {
		src := strings.ToUpper(strings.TrimSpace(c.Get("X-Stream-Source")))
		mode := strings.ToUpper(strings.TrimSpace(c.Get("X-Telemetry-Mode")))
		if src == "MOCK_VIDEO" || mode == "MOCK_SIMULATION" {
			return true
		}
	}
	return false
}

func syncMockTelemetryFromCameraIngress(c *fiber.Ctx, deviceID string, frameID uint32) string {
	videoSrc := strings.Clone(strings.TrimSpace(c.Get("X-Mock-Video-Source")))
	hdrUUID := strings.Clone(strings.TrimSpace(c.Get("X-Mock-Video-UUID")))
	if videoSrc == "" {
		activeSrc, _ := camera_handler.GlobalCameraStore.GetActiveMockVideoMeta()
		videoSrc = activeSrc
	}
	videoUUID := inspection.ResolveMockVideoUUID(videoSrc, hdrUUID, deviceID)
	camera_handler.GlobalCameraStore.SetActiveMockVideoMeta(videoSrc, videoUUID)

	isRewind := c.Get("X-Is-Rewind") == "1" || c.Get("X-Video-Rewind") == "1"
	if isRewind {
		return videoUUID
	}

	nowUnix := time.Now().Unix()
	existing, hasExisting := inspection.GlobalInspectionStore.GetLatestMockTelemetry(deviceID)
	if hasExisting {
		existing.Timestamp = nowUnix
		existing.FrameID = frameID
		existing.VideoUUID = videoUUID
		existing.MockID = videoUUID
		if videoSrc != "" {
			existing.VideoSource = videoSrc
		}
		inspection.GlobalInspectionStore.StoreMockTelemetry(existing)
		return videoUUID
	}

	inspection.GlobalInspectionStore.StoreMockTelemetry(inspection.MockTelemetryPayload{
		MockID:          videoUUID,
		VideoUUID:       videoUUID,
		DeviceID:        deviceID,
		SourceType:      "mock_video",
		TelemetryStruct: "MockTelemetryPayload",
		VideoSource:     videoSrc,
		FrameID:         frameID,
		Timestamp:       nowUnix,
		IsRewind:        false,
		ConnectionMode:  "mock_simulation",
		EnginePipeline:  "YOLOv8n-INT8 + OpenCV HSV",
	})
	return videoUUID
}

func syncTelemetryFromCameraIngress(c *fiber.Ctx, deviceID string, frameID uint32) {
	if deviceID == "" || deviceID == "DEFAULT_CAMERA" || deviceID == "UNKNOWN_DEVICE" {
		return
	}
	if isMockDeviceOrHeader(c, deviceID) {
		_ = syncMockTelemetryFromCameraIngress(c, deviceID, frameID)
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

	inspection.GlobalInspectionStore.StoreTelemetry(inspection.TelemetryIngressPayload{
		DeviceID:        deviceID,
		FirmwareVersion: fwVer,
		DeviceCore:      2,
		Timestamp:       nowUnix,
		Temperature:     realTemp,
		CPUTemperature:  cpuTemp,
		CurrentCPULoad:  cpuLoad,
		IPAddress:       clientIP,
		ConnectionMode:  "wifi_lan",
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
				devID := strings.Clone(payload.DeviceID)
				isMock := isMockDeviceOrHeader(c, devID)
				videoSrc := strings.Clone(strings.TrimSpace(c.Get("X-Mock-Video-Source")))
				hdrUUID := strings.Clone(strings.TrimSpace(c.Get("X-Mock-Video-UUID")))
				videoUUID := ""
				if isMock {
					videoUUID = inspection.ResolveMockVideoUUID(videoSrc, hdrUUID, devID)
				}
				if len(imageBytes) > 0 {
					ipStr := strings.Clone(c.IP())
					isRewind := c.Get("X-Is-Rewind") == "1" || c.Get("X-Video-Rewind") == "1"
					camera_handler.GlobalCameraStore.StoreRawFrameWithMockMeta(devID, payload.FrameID, imageBytes, ipStr, isRewind, videoSrc, videoUUID)
					syncTelemetryFromCameraIngress(c, devID, payload.FrameID)
				}
				if isMock {
					c.Set("X-Stream-Source", "MOCK_VIDEO")
					c.Set("X-Telemetry-Struct", "MockTelemetryPayload")
					c.Set("X-Mock-Video-UUID", videoUUID)
					return c.Status(fiber.StatusCreated).JSON(fiber.Map{
						"status":           "mock_frame_accepted",
						"source_type":      "mock_video",
						"telemetry_struct": "MockTelemetryPayload",
						"device_id":        payload.DeviceID,
						"video_source":     videoSrc,
						"video_uuid":       videoUUID,
						"frame_id":         payload.FrameID,
						"message":          "Mock video frame ingested into shared YOLO CV pipeline",
					})
				}
				c.Set("X-Stream-Source", "LIVE_ESP32")
				c.Set("X-Telemetry-Struct", "TelemetryIngressPayload")
				return c.Status(fiber.StatusCreated).JSON(fiber.Map{
					"status":           "success",
					"source_type":      "live_esp32",
					"telemetry_struct": "TelemetryIngressPayload",
					"device_id":        payload.DeviceID,
					"frame_id":         payload.FrameID,
					"message":          "Live ESP32-S3 camera frame and hardware telemetry ingested",
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
		isMock := isMockDeviceOrHeader(c, deviceID)
		videoSrc := strings.Clone(strings.TrimSpace(c.Get("X-Mock-Video-Source")))
		hdrUUID := strings.Clone(strings.TrimSpace(c.Get("X-Mock-Video-UUID")))
		videoUUID := ""
		if isMock {
			videoUUID = inspection.ResolveMockVideoUUID(videoSrc, hdrUUID, deviceID)
		}
		camera_handler.GlobalCameraStore.StoreRawFrameWithMockMeta(deviceID, uint32(frameID), imageBytes, ipStr, isRewind, videoSrc, videoUUID)
		syncTelemetryFromCameraIngress(c, deviceID, uint32(frameID))

		if isMock {
			c.Set("X-Stream-Source", "MOCK_VIDEO")
			c.Set("X-Telemetry-Struct", "MockTelemetryPayload")
			c.Set("X-Mock-Video-UUID", videoUUID)
		} else {
			c.Set("X-Stream-Source", "LIVE_ESP32")
			c.Set("X-Telemetry-Struct", "TelemetryIngressPayload")
		}
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
		if isMockDeviceOrHeader(nil, activeID) {
			c.Set("X-Stream-Source", "MOCK_VIDEO")
			c.Set("X-Telemetry-Struct", "MockTelemetryPayload")
			videoSrc, videoUUID := camera_handler.GlobalCameraStore.GetActiveMockVideoMeta()
			if mockTel, ok := inspection.GlobalInspectionStore.GetLatestMockTelemetry(activeID); ok {
				if videoSrc == "" {
					videoSrc = mockTel.VideoSource
				}
				if videoUUID == "" {
					videoUUID = mockTel.VideoUUID
				}
			}
			videoUUID = inspection.ResolveMockVideoUUID(videoSrc, videoUUID, activeID)
			c.Set("X-Mock-Video-UUID", videoUUID)
			return c.JSON(fiber.Map{
				"active":           true,
				"device_id":        activeID,
				"source_type":      "mock_video",
				"telemetry_struct": "MockTelemetryPayload",
				"fps":              fps,
				"video_source":     videoSrc,
				"video_uuid":       videoUUID,
				"message":          "Mock video simulation stream active",
			})
		}
		c.Set("X-Stream-Source", "LIVE_ESP32")
		c.Set("X-Telemetry-Struct", "TelemetryIngressPayload")
		camModel := "OV5640"
		if live, ok := inspection.GlobalInspectionStore.GetLatestTelemetry(activeID); ok && live.CameraModel != "" {
			camModel = live.CameraModel
		}
		return c.JSON(fiber.Map{
			"active":           true,
			"device_id":        activeID,
			"source_type":      "live_esp32",
			"telemetry_struct": "TelemetryIngressPayload",
			"fps":              fps,
			"camera_model":     camModel,
			"message":          "Live ESP32-S3 camera stream active",
		})
	})

	camGroup.Post("/mode", func(c *fiber.Ctx) error {
		var req struct {
			Mode        string `json:"mode"`
			VideoSource string `json:"video_source"`
			VideoUUID   string `json:"video_uuid"`
		}
		_ = c.BodyParser(&req)
		mode := strings.ToLower(strings.TrimSpace(req.Mode))
		if mode != "mock" && mode != "live" {
			mode = "live"
		}
		camera_handler.GlobalCameraStore.SetStreamMode(mode)
		if mode == "mock" {
			inspection.GlobalInspectionStore.DisconnectNonMock()
			if req.VideoSource != "" || req.VideoUUID != "" {
				uid := inspection.ResolveMockVideoUUID(req.VideoSource, req.VideoUUID, "MOCK_VIDEO_CAM")
				camera_handler.GlobalCameraStore.SetActiveMockVideoMeta(req.VideoSource, uid)
			}
			_, activeUUID := camera_handler.GlobalCameraStore.GetActiveMockVideoMeta()
			c.Set("X-Stream-Source", "MOCK_VIDEO")
			c.Set("X-Telemetry-Struct", "MockTelemetryPayload")
			if activeUUID != "" {
				c.Set("X-Mock-Video-UUID", activeUUID)
			}
			return c.JSON(fiber.Map{
				"status":            "success",
				"mode":              "mock",
				"video_uuid":        activeUUID,
				"telemetry_struct":  "MockTelemetryPayload",
				"inspection_struct": "MockInspectionRecord",
				"message":           "Switched to Mock Video mode: all live ESP32-S3 streams turned off",
			})
		}
		inspection.GlobalInspectionStore.DisconnectDevice("MOCK_VIDEO_CAM")
		c.Set("X-Stream-Source", "LIVE_ESP32")
		c.Set("X-Telemetry-Struct", "TelemetryIngressPayload")
		return c.JSON(fiber.Map{
			"status":            "success",
			"mode":              "live",
			"telemetry_struct":  "TelemetryIngressPayload",
			"inspection_struct": "InspectionRecord",
			"message":           "Switched to Live ESP32-S3 mode: live hardware streams enabled",
		})
	})

	camGroup.Get("/mode", func(c *fiber.Ctx) error {
		mode := camera_handler.GlobalCameraStore.GetStreamMode()
		if mode == "mock" {
			c.Set("X-Stream-Source", "MOCK_VIDEO")
			c.Set("X-Telemetry-Struct", "MockTelemetryPayload")
		} else {
			c.Set("X-Stream-Source", "LIVE_ESP32")
			c.Set("X-Telemetry-Struct", "TelemetryIngressPayload")
		}
		return c.JSON(fiber.Map{
			"mode": mode,
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
