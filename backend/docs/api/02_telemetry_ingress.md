# Module 02: Edge Telemetry & Sensor Data Ingress

This module tests the ingress pipeline for edge IoT nodes (ESP32-S3 microcontroller units equipped with BME680 environmental sensors).

---

## 1. ESP32-S3 Telemetry Dispatch

Simulates environmental metric packets broadcast by the edge inspection device.

### Ingress Metrics:
- **`device_id`**: Unique hardware identifier (MAC / device serial string).
- **`firmware_version`**: Active ESP-IDF firmware build version.
- **`temperature`**: ESP32-S3 internal temperature in Celsius (°C).
- **`cpu_load`**: Edge CPU core utilization percentage (0 - 100%).
- **`timestamp`**: Epoch timestamp (seconds).

### Request
```bash
curl -i -X POST http://localhost:8080/api/v1/telemetry \
  -H "Content-Type: application/json" \
  -d '{
    "device_id": "ESP32-S3-NODE-01",
    "device_name": "Optical Inspection Pod Alpha",
    "firmware_version": "1.0.4-prod",
    "device_core": 2,
    "temperature": 41.25,
    "cpu_load": 22,
    "timestamp": 1758412800
  }'
```

### Expected Response (`201 Created`)
```json
{
  "status": "success",
  "id": "e9b2f3a1-5678-4321-abcd-ef0123456789"
}
```

---

## 2. Telemetry Ingress Validation (Edge Cases)

### Test with Malformed JSON
```bash
curl -i -X POST http://localhost:8080/api/v1/telemetry \
  -H "Content-Type: application/json" \
  -d '{"device_id": "ESP32-S3-NODE-01", "temperature": "INVALID_FLOAT"}'
```

### Expected Response (`400 Bad Request`)
```json
{
  "error": "Invalid JSON payload"
}
```

