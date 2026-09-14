package api

import (
	"errors"
	"net/http"

	"bawal_vision_backend/cmd/internal/inspection"

	fiber "github.com/gofiber/fiber/v2"
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
}
func (h *Handler) GetRecords(c *fiber.Ctx) error {
	var records []inspection.InspectionRecord

	err := h.db.
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

func (h *Handler) GetRecordByID(c *fiber.Ctx) error {
	id := c.Params("id")
	var record inspection.InspectionRecord

	if err := h.db.First(&record, "id = ?", id).Error; err != nil {
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
