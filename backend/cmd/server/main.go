package main

import (
	"bawal_vision_backend/internal/api"
	"bawal_vision_backend/internal/camera_handler"
	"bawal_vision_backend/internal/device"
	"bawal_vision_backend/internal/food"
	"bawal_vision_backend/internal/inspection"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/gofiber/fiber/v2"
	"github.com/gofiber/fiber/v2/middleware/cors"
	"github.com/gofiber/fiber/v2/middleware/logger"
	"github.com/gofiber/fiber/v2/middleware/recover"
	"gorm.io/driver/postgres"
	"gorm.io/gorm"
)

var version = "dev"

func getEnv(key, fallback string) string {
	if value, exists := os.LookupEnv(key); exists {
		return value
	}
	return fallback
}

func main() {
	host := getEnv("DB_HOST", "postgres")
	user := getEnv("DB_USER", "postgres")
	password := getEnv("DB_PASSWORD", "postgres")
	dbname := getEnv("DB_NAME", "bawal_vision")
	port := getEnv("DB_PORT", "5432")
	sslmode := getEnv("DB_SSLMODE", "disable")

	dsn := fmt.Sprintf(
		"host=%s user=%s password=%s dbname=%s port=%s sslmode=%s",
		host, user, password, dbname, port, sslmode,
	)

	db, err := gorm.Open(postgres.Open(dsn), &gorm.Config{})
	if err != nil {
		log.Fatalf("Failed to connect to database: %v", err)
	}

	if err := db.AutoMigrate(
		&device.Device{},
		&food.FoodItem{},
		&inspection.InspectionRecord{},
		&inspection.InspectionFood{},
		&camera_handler.CameraFrame{},
	); err != nil {
		log.Fatalf("Database migration failed: %v", err)
	}

	// Clean up legacy columns if they exist in existing database volumes and ensure new sensor columns exist
	_ = db.Exec("ALTER TABLE inspections DROP COLUMN IF EXISTS user_id")
	_ = db.Exec("DROP TABLE IF EXISTS users CASCADE")
	_ = db.Exec("ALTER TABLE inspections DROP COLUMN IF EXISTS humidity, DROP COLUMN IF EXISTS gas_voc, DROP COLUMN IF EXISTS pressure")
	_ = db.Exec("ALTER TABLE inspections ADD COLUMN IF NOT EXISTS cpu_temperature NUMERIC(5, 2) NOT NULL DEFAULT 35.00")
	_ = db.Exec("ALTER TABLE inspections ADD COLUMN IF NOT EXISTS lux_luminance NUMERIC(5, 2) NOT NULL DEFAULT 120.00")
	_ = db.Exec("ALTER TABLE inspections ADD COLUMN IF NOT EXISTS white_balance_status VARCHAR(32) NOT NULL DEFAULT 'BALANCED'")
	_ = db.Exec("ALTER TABLE inspections ADD COLUMN IF NOT EXISTS color_temperature INT NOT NULL DEFAULT 6500")
	_ = db.Exec("ALTER TABLE inspection_foods DROP COLUMN IF EXISTS weight_grams")
	_ = db.Exec("ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS temperature NUMERIC(5, 2) NOT NULL DEFAULT 25.00")
	_ = db.Exec("ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS similarity_rate NUMERIC(5, 4) NOT NULL DEFAULT 0.0000")
	_ = db.Exec("ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS spoilage_score NUMERIC(5, 2) NOT NULL DEFAULT 0.00")
	_ = db.Exec("ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS status VARCHAR(16) NOT NULL DEFAULT 'PASS'")
	_ = db.Exec("DELETE FROM inspections WHERE id NOT IN (SELECT DISTINCT inspection_id FROM inspection_foods)")
	// Deduplicate legacy MOCK_VIDEO_CAM rows created before per-video deterministic UUID assignment
	_ = db.Exec(`DELETE FROM inspection_foods WHERE inspection_id IN (
		SELECT id FROM (
			SELECT id, ROW_NUMBER() OVER (PARTITION BY device_id, COALESCE(image_s3_uri, '') ORDER BY created_at DESC) AS rn
			FROM inspections
			WHERE device_id = 'MOCK_VIDEO_CAM' OR device_id LIKE 'MOCK_%'
		) sub WHERE sub.rn > 1
	)`)
	_ = db.Exec(`DELETE FROM inspections WHERE id IN (
		SELECT id FROM (
			SELECT id, ROW_NUMBER() OVER (PARTITION BY device_id, COALESCE(image_s3_uri, '') ORDER BY created_at DESC) AS rn
			FROM inspections
			WHERE device_id = 'MOCK_VIDEO_CAM' OR device_id LIKE 'MOCK_%'
		) sub WHERE sub.rn > 1
	)`)
	// Ensure mock video records never store ESP32 hardware values in the database
	_ = db.Exec(`UPDATE inspections SET firmware_version = '', temperature = 0.0, cpu_temperature = 0.0, cpu_load = 0
		WHERE device_id = 'MOCK_VIDEO_CAM' OR device_id LIKE 'MOCK_%' OR COALESCE(image_s3_uri, '') LIKE '%.mp4'`)
	_ = db.Exec(`UPDATE devices SET firmware_version = '', device_type = 'MOCK_VIDEO'
		WHERE id = 'MOCK_VIDEO_CAM' OR id LIKE 'MOCK_%'`)
	if err := db.Exec(`DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = current_schema()
          AND table_name = 'inspections'
          AND column_name = 'food_temperature'
    ) THEN
        UPDATE inspections SET temperature = food_temperature;
        ALTER TABLE inspections DROP COLUMN food_temperature;
    END IF;
END $$;`).Error; err != nil {
		log.Fatalf("Database migration failed while removing legacy food_temperature column: %v", err)
	}

	// Initialize Asynchronous Inspection Batch Worker (Buffer: 512, Batch: 25, Flush: 1s)
	batchWorker := inspection.InitGlobalBatchWorker(db, 512, 25, 1*time.Second)

	app := fiber.New(fiber.Config{
		AppName:      fmt.Sprintf("BawalVision Gateway v%s", version),
		ServerHeader: "Bawal-Vision-Backend",
	})

	app.Use(recover.New())
	app.Use(cors.New(cors.Config{
		AllowOrigins:  "*",
		AllowMethods:  "GET,POST,PUT,PATCH,DELETE,HEAD,OPTIONS",
		AllowHeaders:  "Origin,Content-Type,Accept,Authorization,X-Device-IP,X-Device-ID,X-Frame-ID,X-ESP-Temp,X-ESP-CPU-Load,X-Camera-Model,X-Firmware-Version,X-Stream-Source,X-Telemetry-Mode,X-Telemetry-Struct,X-Mock-Video-Source,X-Mock-Video-UUID,X-Is-Rewind,X-Video-Rewind",
		ExposeHeaders: "X-Stream-Source,X-Telemetry-Struct,X-Mock-Video-UUID",
	}))
	app.Use(logger.New(logger.Config{
		Next: func(c *fiber.Ctx) bool {
			path := c.Path()
			// Skip repetitive high-frequency camera streaming & frame ingress to eliminate stdout / disk I/O bottlenecks
			return strings.HasPrefix(path, "/api/v1/camera/latest") ||
				path == "/api/v1/camera/frame" ||
				path == "/api/v1/camera/annotated" ||
				path == "/api/v1/camera/raw" ||
				path == "/api/v1/camera/stream"
		},
	}))

	apiGroup := app.Group("/api/v1")
	app.Get("/api/v1/health", func(c *fiber.Ctx) error {
		return c.JSON(fiber.Map{
			"status":  "ok",
			"service": "bawal-vision-backend",
			"version": version,
		})
	})
	api.RegisterDeviceRoutes(apiGroup, db)
	api.RegisterFrontendRoutes(apiGroup, db)
	api.RegisterCameraRoutes(apiGroup, db)

	// Allow ESP32-S3 nodes to rediscover this backend over UDP broadcast when
	// their provisioned BACKEND_IP becomes stale after a DHCP/Wi-Fi change.
	device.StartBackendDiscovery(version)

	// Direct alias for /examples/* to /api/v1/examples/file/*
	app.Get("/examples/*", func(c *fiber.Ctx) error {
		return c.Redirect("/api/v1/examples/file/" + c.Params("*"))
	})

	// Graceful shutdown handling
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	go func() {
		<-sigChan
		log.Println("[SHUTDOWN] Signal received, shutting down gracefully...")
		if err := app.Shutdown(); err != nil {
			log.Printf("[SHUTDOWN ERROR] Fiber app shutdown failed: %v", err)
		}
		batchWorker.Stop()
		log.Println("[SHUTDOWN] Batch worker flushed and stopped.")
	}()

	log.Printf("Server running on http://0.0.0.0:8080 (version: %s)...", version)
	if err := app.Listen("0.0.0.0:8080"); err != nil {
		log.Printf("[SERVER] Server stopped: %v", err)
	}
}
