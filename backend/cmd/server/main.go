package main

import (
	"fmt"
	"log"
	"os"
	"time"

	"bawal_vision_backend/cmd/internal/inspection"

	fiber "github.com/gofiber/fiber/v2"
	"github.com/google/uuid"
	"gorm.io/driver/postgres"
	"gorm.io/gorm"
)

func RegisterDeviceRoutes(router fiber.Router, db *gorm.DB) {
	// Endpoint resmi yang dipanggil ESP32-S3: POST /api/v1/telemetry
	router.Post("/telemetry", func(c *fiber.Ctx) error {
		var payload inspection.TelemetryIngressPayload

		if err := c.BodyParser(&payload); err != nil {
			log.Printf("[INGRESS ERROR] Failed to parse JSON: %v", err)
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{
				"error": "Invalid JSON payload",
			})
		}

		record := inspection.InspectionRecord{
			ID:        uuid.New().String(),
			DeviceID:  payload.DeviceID,
			CreatedAt: time.Now(),
			Telemetry: payload,
		}

		if err := db.Create(&record).Error; err != nil {
			log.Printf("[DB ERROR] Failed to save record: %v", err)
			return c.Status(fiber.StatusInternalServerError).JSON(fiber.Map{
				"error": "Failed to save data to database",
			})
		}

		log.Printf("[INGRESS SUCCESS] Received telemetry from Device: %s", payload.DeviceID)

		return c.Status(fiber.StatusCreated).JSON(fiber.Map{
			"status": "acknowledged",
			"id":     record.ID,
		})
	})
}

func getEnv(key, fallback string) string {
	if val, ok := os.LookupEnv(key); ok {
		return val
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

	if err := db.AutoMigrate(&inspection.InspectionRecord{}); err != nil {
		log.Fatalf("Database migration failed: %v", err)
	}

	app := fiber.New(fiber.Config{
		ServerHeader: "Bawal-Vision-Backend",
	})

	// Grouping Route API v1
	apiGroup := app.Group("/api/v1")
	RegisterDeviceRoutes(apiGroup, db)

	log.Println("Server running on http://0.0.0.0:8080...")
	log.Fatal(app.Listen(":8080"))
}
