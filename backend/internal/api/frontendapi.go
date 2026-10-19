package api

import (
	"errors"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"

	"bawal_vision_backend/internal/inspection"

	fiber "github.com/gofiber/fiber/v2"
	"gorm.io/gorm"
)

var (
	globalThresholdMutex sync.RWMutex
	globalConfidenceThreshold = initDefaultThreshold()
)

func initDefaultThreshold() float64 {
	if env := os.Getenv("CONF_THRESHOLD"); env != "" {
		if val, err := strconv.ParseFloat(env, 64); err == nil && val >= 0.25 && val <= 0.98 {
			return val
		}
	}
	return 0.35
}

type Handler struct {
	db *gorm.DB
}

func NewHandler(db *gorm.DB) *Handler {
	return &Handler{db: db}
}

func RegisterFrontendRoutes(router fiber.Router, db *gorm.DB) {
	h := NewHandler(db)

	router.Get("/records", h.GetRecords)
	router.Get("/records/:id", h.GetRecordByID)
	router.Get("/system/engines", h.GetEngineStatus)
	router.Get("/system/threshold", h.GetThreshold)
	router.Post("/system/threshold", h.SetThreshold)
	router.Get("/examples", h.GetExampleVideos)
	router.Get("/examples/videos", h.GetExampleVideos)
	router.Get("/examples/file/*", h.ServeExampleVideo)
	router.Get("/examples/file/:filename", h.ServeExampleVideo)
	router.Get("/examples/*", h.ServeExampleVideo)
}

func (h *Handler) GetThreshold(c *fiber.Ctx) error {
	globalThresholdMutex.RLock()
	t := globalConfidenceThreshold
	globalThresholdMutex.RUnlock()
	return c.JSON(fiber.Map{
		"threshold": t,
		"min":       0.25,
		"max":       0.98,
		"default":   0.35,
	})
}

func (h *Handler) SetThreshold(c *fiber.Ctx) error {
	var req struct {
		Threshold float64 `json:"threshold"`
	}
	if err := c.BodyParser(&req); err != nil {
		return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{"error": "Invalid request body"})
	}
	if req.Threshold < 0.25 {
		req.Threshold = 0.25
	} else if req.Threshold > 0.98 {
		req.Threshold = 0.98
	}
	globalThresholdMutex.Lock()
	globalConfidenceThreshold = req.Threshold
	globalThresholdMutex.Unlock()

	return c.JSON(fiber.Map{
		"success":   true,
		"threshold": req.Threshold,
	})
}

func (h *Handler) GetExampleVideos(c *fiber.Ctx) error {
	paths := []string{"./examples", "../examples", "../../examples", "../../../examples", "./frontend/static/examples", "../frontend/static/examples", "/app/examples"}
	foundMap := make(map[string]bool)
	videoFiles := make([]string, 0)

	for _, basePath := range paths {
		if _, err := os.Stat(basePath); err != nil {
			continue
		}
		_ = filepath.WalkDir(basePath, func(path string, d os.DirEntry, err error) error {
			if err != nil || d.IsDir() {
				return nil
			}
			lower := strings.ToLower(d.Name())
			if strings.HasSuffix(lower, ".mp4") || strings.HasSuffix(lower, ".webm") {
				rel, err := filepath.Rel(basePath, path)
				if err == nil {
					rel = filepath.ToSlash(rel)
					if !foundMap[rel] {
						foundMap[rel] = true
						videoFiles = append(videoFiles, rel)
					}
				}
			}
			return nil
		})
	}

	return c.JSON(fiber.Map{
		"status": "success",
		"videos": videoFiles,
	})
}

func (h *Handler) ServeExampleVideo(c *fiber.Ctx) error {
	filename := c.Params("*")
	if filename == "" {
		filename = c.Params("filename")
	}
	if filename == "" {
		return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{"error": "Filename required"})
	}

	cleaned := filepath.Clean(filename)
	if strings.HasPrefix(cleaned, "..") || filepath.IsAbs(cleaned) {
		return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{"error": "Invalid file path"})
	}

	paths := []string{
		filepath.Join("./examples", cleaned),
		filepath.Join("../examples", cleaned),
		filepath.Join("../../examples", cleaned),
		filepath.Join("../../../examples", cleaned),
		filepath.Join("./frontend/static/examples", cleaned),
		filepath.Join("../frontend/static/examples", cleaned),
		filepath.Join("/app/examples", cleaned),
	}

	for _, p := range paths {
		if fi, err := os.Stat(p); err == nil && !fi.IsDir() {
			return c.SendFile(p)
		}
	}

	return c.Status(fiber.StatusNotFound).JSON(fiber.Map{"error": "Video file not found"})
}

func (h *Handler) GetRecords(c *fiber.Ctx) error {
	var records []inspection.InspectionRecord

	err := h.db.
		Preload("InspectionFoods.FoodItem").
		Order("created_at DESC").
		Limit(100).
		Find(&records).Error

	if err != nil {
		return c.Status(http.StatusInternalServerError).JSON(fiber.Map{
			"error": "Failed to retrieve inspection records",
		})
	}

	return c.JSON(records)
}

func (h *Handler) GetEngineStatus(c *fiber.Ctx) error {
	arch := runtime.GOARCH
	gravitonEnv := os.Getenv("AWS_GRAVITON_AVAILABLE") == "true" || os.Getenv("ENABLE_GRAVITON") == "true"
	isGraviton := (arch == "arm64" || gravitonEnv)

	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	serverAllocMB := float64(m.Alloc) / (1024 * 1024)
	serverSysMB := float64(m.Sys) / (1024 * 1024)

	var hostUsedMB float64
	var hostTotalMB float64
	if data, err := os.ReadFile("/proc/meminfo"); err == nil {
		lines := strings.Split(string(data), "\n")
		var totalKB, availKB float64
		for _, line := range lines {
			if strings.HasPrefix(line, "MemTotal:") {
				fields := strings.Fields(line)
				if len(fields) >= 2 {
					totalKB, _ = strconv.ParseFloat(fields[1], 64)
				}
			} else if strings.HasPrefix(line, "MemAvailable:") {
				fields := strings.Fields(line)
				if len(fields) >= 2 {
					availKB, _ = strconv.ParseFloat(fields[1], 64)
				}
			}
		}
		if totalKB > 0 && availKB > 0 {
			hostTotalMB = totalKB / 1024.0
			hostUsedMB = (totalKB - availKB) / 1024.0
		}
	}

	return c.JSON(fiber.Map{
		"arch":                   arch,
		"num_cpu":                runtime.NumCPU(),
		"parallel_cores":         runtime.NumCPU(),
		"graviton_available":     isGraviton,
		"local_engine_available": true,
		"server_mem_alloc_mb":    math.Round(serverAllocMB*10) / 10,
		"server_mem_sys_mb":      math.Round(serverSysMB*10) / 10,
		"host_mem_used_mb":       math.Round(hostUsedMB),
		"host_mem_total_mb":      math.Round(hostTotalMB),
		"memory_usage_mb":        math.Round(serverSysMB*10) / 10,
	})
}

func (h *Handler) GetRecordByID(c *fiber.Ctx) error {
	id := c.Params("id")
	var record inspection.InspectionRecord

	if err := h.db.Preload("InspectionFoods.FoodItem").First(&record, "id = ?", id).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return c.Status(http.StatusNotFound).JSON(fiber.Map{
				"error": "Inspection record not found",
			})
		}
		return c.Status(http.StatusInternalServerError).JSON(fiber.Map{
			"error": "Failed to query database",
		})
	}

	return c.JSON(record)
}
