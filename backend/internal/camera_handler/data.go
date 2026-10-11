package camera_handler

import (
	"bawal_vision_backend/internal/model"
)

// Re-export centralized camera models and formats for modularity and backward compatibility
type FrameFormat = model.FrameFormat

const (
	FormatJPEG      = model.FormatJPEG
	FormatRGB565    = model.FormatRGB565
	FormatRGB888    = model.FormatRGB888
	FormatGrayscale = model.FormatGrayscale
	FormatRaw       = model.FormatRaw
)

type CameraFrame = model.CameraFrame
type FrameIngressPayload = model.FrameIngressPayload
type FrameQueryResponse = model.FrameQueryResponse
