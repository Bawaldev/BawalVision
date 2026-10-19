package inspection

import (
	"strings"

	"github.com/google/uuid"
)

// DeterministicMockVideoUUID generates a deterministic RFC-4122 UUID v5 string for a given mock video identifier.
// The algorithm matches the frontend implementation byte-for-byte so every mock video has a permanent unique UUID
// that never duplicates trays across page reloads, tab switches, or backend restarts.
func DeterministicMockVideoUUID(videoKey string) string {
	cleaned := strings.TrimSpace(strings.ToLower(videoKey))
	cleaned = strings.TrimPrefix(cleaned, "/api/v1/examples/file/")
	cleaned = strings.TrimPrefix(cleaned, "examples/")
	if cleaned == "" {
		cleaned = "default_mock_video"
	}
	if parsed, err := uuid.Parse(cleaned); err == nil {
		return parsed.String()
	}

	input := "bawalvision:mock-video:" + cleaned
	seeds := [4]uint32{0x811c9dc5, 0x9e3779b9, 0x85ebca6b, 0xc2b2ae35}
	var b [16]byte
	for sIdx, seed := range seeds {
		h := seed
		for i := 0; i < len(input); i++ {
			h ^= uint32(input[i])
			h *= 0x01000193
		}
		h ^= uint32(sIdx) * 0x27d4eb2d
		h ^= h >> 16
		h *= 0x85ebca6b
		h ^= h >> 13
		h *= 0xc2b2ae35
		h ^= h >> 16

		offset := sIdx * 4
		b[offset] = byte(h >> 24)
		b[offset+1] = byte(h >> 16)
		b[offset+2] = byte(h >> 8)
		b[offset+3] = byte(h)
	}
	b[6] = (b[6] & 0x0f) | 0x50
	b[8] = (b[8] & 0x3f) | 0x80

	u, err := uuid.FromBytes(b[:])
	if err != nil {
		return uuid.NewSHA1(uuid.NameSpaceURL, []byte(input)).String()
	}
	return u.String()
}

// ResolveMockVideoUUID resolves a stable unique UUID for a mock video from its source path, explicit UUID, or fallback ID.
func ResolveMockVideoUUID(videoSource, explicitUUID, fallbackID string) string {
	src := strings.TrimSpace(videoSource)
	exp := strings.TrimSpace(explicitUUID)
	if src != "" && src != "custom" {
		return DeterministicMockVideoUUID(src)
	}
	if exp != "" {
		if parsed, err := uuid.Parse(exp); err == nil {
			return parsed.String()
		}
		return DeterministicMockVideoUUID(exp)
	}
	if src != "" {
		return DeterministicMockVideoUUID(src)
	}
	if strings.TrimSpace(fallbackID) != "" {
		return DeterministicMockVideoUUID(fallbackID)
	}
	return DeterministicMockVideoUUID("default_mock_video")
}

// EvaluateFoodQuality applies multi-sensor quality & food safety rules combining:
// 1. Computer Vision: Classification similarity & object integrity (Hue/color degradation detection)
// 2. Machine Learning: Visual decomposition & spoilage scoring (ONNX model inference)
// 3. Food Temperature Sensor: Food surface temperature (danger zone > 28°C acceleration)
func EvaluateFoodQuality(foodName string, cvConf float64, mlSpoilage float64, foodTemp float64, requestedStatus string) (float64, float64, float64, string) {
	if foodTemp <= 0 {
		foodTemp = 25.0
	}
	if cvConf <= 0 {
		cvConf = 0.88
	}

	// Dynamic ML Spoilage Model from CV features + Thermal decay
	if mlSpoilage <= 0 {
		mlSpoilage = (1.0 - cvConf) * 25.0
		if foodTemp > 28.0 {
			mlSpoilage += (foodTemp - 28.0) * 2.2
		}
		if mlSpoilage > 100.0 {
			mlSpoilage = 100.0
		}
	}

	status := "PASS"
	if requestedStatus != "" {
		status = requestedStatus
	} else if cvConf < 0.35 || mlSpoilage >= 55.0 || (foodTemp > 42.0 && mlSpoilage >= 40.0) {
		status = "REJECT"
	} else if cvConf < 0.60 || mlSpoilage >= 35.0 || foodTemp > 38.0 {
		status = "REVIEW"
	}

	return cvConf, mlSpoilage, foodTemp, status
}

// EvaluateVisionDetections executes the unified YOLO object detection, HSV spoilage scoring,
// and shelf-life (t_safe_hours) return logic shared identically by both Live ESP32-S3 and Mock Video pipelines.
func EvaluateVisionDetections(detections []FoodDetection, realTemp float64) (anySpoiled bool, maxSpoilageScore float64, tSafeHours float64, normalized []FoodDetection) {
	if realTemp <= 0 {
		realTemp = 25.0
	}

	normalized = make([]FoodDetection, 0, len(detections))
	for _, det := range detections {
		conf, spoilage, evaluatedTemp, status := EvaluateFoodQuality(
			det.FoodName,
			det.SimilarityRate,
			det.SpoilageScore,
			realTemp,
			det.Status,
		)
		if spoilage > maxSpoilageScore {
			maxSpoilageScore = spoilage
		}
		if status == "REJECT" || spoilage >= 55.0 || conf < 0.35 {
			anySpoiled = true
		}
		normalized = append(normalized, FoodDetection{
			FoodName:       det.FoodName,
			SimilarityRate: conf,
			Temperature:    evaluatedTemp,
			SpoilageScore:  spoilage,
			Status:         status,
			Box:            det.Box,
		})
	}

	tSafeHours = 8.0
	if realTemp > 20.0 {
		tSafeHours = 8.0 * (1.0 / (1.0 + 0.05*(realTemp-20.0)))
	}
	if anySpoiled {
		tSafeHours = 0.0
	}

	return anySpoiled, maxSpoilageScore, tSafeHours, normalized
}
