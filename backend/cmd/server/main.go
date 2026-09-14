package main

import (
	"log"

	"bawal_vision_backend/cmd/internal/api"
	"bawal_vision_backend/cmd/internal/inspection"

	fiber "github.com/gofiber/fiber/v2"
	"gorm.io/driver/postgres"
	"gorm.io/gorm"
)

func main() {
	dsn := "host=localhost user=postgres password=postgres dbname=bawal_vision port=5432 sslmode=disable"
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

	store := inspection.NewInspectionStore()

	app.Post("/api/v1/telemetry", func(c *fiber.Ctx) error {
		payload, err := store.ProcessTelemetry(c.Request().BodyStream())
		if err != nil {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{
				"error": err.Error(),
			})
		}

		return c.Status(fiber.StatusOK).JSON(fiber.Map{
			"status": "acknowledged",
			"data":   payload,
		})
	})

	apiGroup := app.Group("/api/v1")
	api.RegisterFrontendRoutes(apiGroup, db)

	log.Println("Server running on http://0.0.0.0:8080...")
	log.Fatal(app.Listen(":8080"))
}
