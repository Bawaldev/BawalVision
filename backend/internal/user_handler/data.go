package user_handler

import (
	"bawal_vision_backend/internal/model"
)

// Re-export centralized user models for modularity and backward compatibility
type User = model.User
type UserTelemetry = model.UserTelemetry
type RegisterRequest = model.RegisterRequest
type LoginRequest = model.LoginRequest
