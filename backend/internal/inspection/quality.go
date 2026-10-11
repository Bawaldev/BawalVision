package inspection

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

