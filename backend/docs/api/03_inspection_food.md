# Module 03: Inspection Logs & Food Classification Records

This module covers querying inspection records, historical inference logs, and associated food items via the frontend API layer.

---

## 1. Query All Inspection Records (Authenticated)

Fetches the latest 100 inspection records ordered by timestamp (`created_at DESC`).

> **Note:** The `-b .cache_trash/cookies.txt` flag forwards the active session cookie in the HTTP request header (`Cookie: session=...`).

### Request
```bash
curl -i -X GET http://localhost:8080/api/v1/records \
  -b .cache_trash/cookies.txt
```

### Expected Response (`200 OK`)
```json
[
  {
    "id": "e9b2f3a1-5678-4321-abcd-ef0123456789",
    "device_id": "ESP32-S3-NODE-01",
    "firmware_version": "1.0.4-prod",
    "temperature": 41.25,
    "cpu_load": 22,
    "is_spoiled": false,
    "t_safe_hours": 0.0,
    "created_at": "2026-09-21T09:15:00Z"
  }
]
```

---

## 2. Query Inspection Record by ID

Retrieves a single inspection record using its unique UUID identifier.

### Request
```bash
curl -i -X GET http://localhost:8080/api/v1/records/e9b2f3a1-5678-4321-abcd-ef0123456789 \
  -b .cache_trash/cookies.txt
```

### Expected Response (`200 OK`)
```json
{
  "id": "e9b2f3a1-5678-4321-abcd-ef0123456789",
  "device_id": "ESP32-S3-NODE-01",
  "firmware_version": "1.0.4-prod",
  "temperature": 41.25,
  "cpu_load": 22,
  "is_spoiled": false,
  "t_safe_hours": 0.0,
  "created_at": "2026-09-21T09:15:00Z"
}
```

### Negative Test: Record Not Found
```bash
curl -i -X GET http://localhost:8080/api/v1/records/00000000-0000-0000-0000-000000000000 \
  -b .cache_trash/cookies.txt
```

### Expected Response (`404 Not Found`)
```json
{
  "error": "Inspection record not found"
}
```

