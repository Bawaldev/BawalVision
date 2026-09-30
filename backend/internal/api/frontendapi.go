package api

import (
	"errors"
	"net/http"
	"os"
	"runtime"
	"time"

	"bawal_vision_backend/internal/inspection"
	"bawal_vision_backend/internal/user_handler"

	fiber "github.com/gofiber/fiber/v2"
	"golang.org/x/crypto/bcrypt"
	"gorm.io/gorm"
)

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

	return c.JSON(fiber.Map{
		"arch":                   arch,
		"graviton_available":     isGraviton,
		"local_engine_available": true,
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

func RegisterUserHandler(db *gorm.DB) fiber.Handler {
	return func(c *fiber.Ctx) error {
		var payload user_handler.RegisterRequest
		if err := c.BodyParser(&payload); err != nil {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{
				"error": "Invalid request payload",
			})
		}

		if payload.Email == "" || payload.Password == "" {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{
				"error": "Email and password are required",
			})
		}

		hashedPassword, err := bcrypt.GenerateFromPassword([]byte(payload.Password), bcrypt.DefaultCost)
		if err != nil {
			return c.Status(fiber.StatusInternalServerError).JSON(fiber.Map{
				"error": "Failed to hash password",
			})
		}

		newUser := user_handler.User{
			Name:     payload.Name,
			Email:    payload.Email,
			Password: string(hashedPassword),
		}
		if err := db.Create(&newUser).Error; err != nil {
			return c.Status(fiber.StatusConflict).JSON(fiber.Map{
				"error": "Email already exists or database insertion failed",
			})
		}

		return c.Status(fiber.StatusCreated).JSON(fiber.Map{
			"message": "User registered successfully",
			"user_id": newUser.ID,
		})
	}
}

func LoginUserHandler(db *gorm.DB) fiber.Handler {
	return func(c *fiber.Ctx) error {
		var req user_handler.LoginRequest
		if err := c.BodyParser(&req); err != nil {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{
				"error": "Invalid request payload",
			})
		}

		if req.Email == "" || req.Password == "" {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{
				"error": "Email and password are required",
			})
		}

		var foundUser user_handler.User
		if err := db.Where("email = ?", req.Email).First(&foundUser).Error; err != nil {
			return c.Status(fiber.StatusUnauthorized).JSON(fiber.Map{
				"error": "Invalid email or password",
			})
		}

		err := bcrypt.CompareHashAndPassword([]byte(foundUser.Password), []byte(req.Password))
		if err != nil {
			return c.Status(fiber.StatusUnauthorized).JSON(fiber.Map{
				"error": "Invalid email or password",
			})
		}

		telemetry := user_handler.UserTelemetry{
			UserID:    foundUser.ID,
			Email:     foundUser.Email,
			Event:     "user_login",
			Timestamp: time.Now(),
			IPAddress: c.IP(),
			UserAgent: c.Get("User-Agent"),
		}
		_ = telemetry

		if err := user_handler.Session.SetSessionCookie(c, foundUser.ID, 24*time.Hour); err != nil {
			return c.Status(fiber.StatusInternalServerError).JSON(fiber.Map{
				"error": "Failed to generate session cookie",
			})
		}

		return c.Status(fiber.StatusOK).JSON(fiber.Map{
			"message": "Login successful",
			"user_id": foundUser.ID,
		})
	}
}

func RegisterUserRoutes(router fiber.Router, db *gorm.DB) {
	authGroup := router.Group("/auth")
	authGroup.Post("/register", RegisterUserHandler(db))
	authGroup.Post("/login", LoginUserHandler(db))
}
