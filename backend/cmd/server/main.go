package main

import (
	"bawal_vision_backend/internal/api"
	"bawal_vision_backend/internal/camera_handler"
	"bawal_vision_backend/internal/device"
	"bawal_vision_backend/internal/food"
	"bawal_vision_backend/internal/inspection"
	"bawal_vision_backend/internal/user_handler"
	"fmt"
	"log"
	"os"

	"github.com/gofiber/fiber/v2"
	"github.com/gofiber/fiber/v2/middleware/logger"
	"github.com/gofiber/fiber/v2/middleware/recover"
	"gorm.io/driver/postgres"
	"gorm.io/gorm"
)

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
		&user_handler.User{},
		&device.Device{},
		&food.FoodItem{},
		&inspection.InspectionRecord{},
		&inspection.InspectionFood{},
		&camera_handler.CameraFrame{},
	); err != nil {
		log.Fatalf("Database migration failed: %v", err)
	}

	// Clean up legacy columns if they exist in existing database volumes and ensure new sensor columns exist
	_ = db.Exec("ALTER TABLE inspections DROP COLUMN IF EXISTS humidity, DROP COLUMN IF EXISTS gas_voc, DROP COLUMN IF EXISTS pressure")
	_ = db.Exec("ALTER TABLE inspections ADD COLUMN IF NOT EXISTS food_temperature NUMERIC(5, 2) NOT NULL DEFAULT 25.00")
	_ = db.Exec("ALTER TABLE inspections ADD COLUMN IF NOT EXISTS cpu_temperature NUMERIC(5, 2) NOT NULL DEFAULT 35.00")
	_ = db.Exec("ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS temperature NUMERIC(5, 2) NOT NULL DEFAULT 25.00")
	_ = db.Exec("ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS similarity_rate NUMERIC(5, 4) NOT NULL DEFAULT 0.0000")
	_ = db.Exec("ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS spoilage_score NUMERIC(5, 2) NOT NULL DEFAULT 0.00")
	_ = db.Exec("ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS status VARCHAR(16) NOT NULL DEFAULT 'PASS'")
	_ = db.Exec("DELETE FROM inspections WHERE id NOT IN (SELECT DISTINCT inspection_id FROM inspection_foods)")

	app := fiber.New(fiber.Config{
		ServerHeader: "Bawal-Vision-Backend",
	})

	app.Use(recover.New())
	app.Use(logger.New())

	apiGroup := app.Group("/api/v1")
	api.RegisterUserRoutes(apiGroup, db)
	api.RegisterDeviceRoutes(apiGroup, db)
	api.RegisterFrontendRoutes(apiGroup, db)
	api.RegisterCameraRoutes(apiGroup, db)

	log.Println("Server running on http://0.0.0.0:8080...")
	log.Fatal(app.Listen(":8080"))
}
