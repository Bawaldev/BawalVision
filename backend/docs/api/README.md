# Backend API Testing Suite & cURL Manual

This directory contains modular test suites and cURL documentation designed for verification, competition benchmarking (OpenCV AI Competition 2026), and developer onboarding.

---

## 📑 Test Modules Index

| Module | Document | Description |
| :--- | :--- | :--- |
| **01. Authentication & Sessions** | [01_auth_sessions.md](01_auth_sessions.md) | User registration, login, cryptographic key validation, and encrypted session cookie management. |
| **02. Edge Telemetry Ingress** | [02_telemetry_ingress.md](02_telemetry_ingress.md) | ESP32-S3 IoT sensor data streaming (temperature, humidity, VOC gas, atmospheric pressure, CPU load). |
| **03. Inspection & Food Records** | [03_inspection_food.md](03_inspection_food.md) | Querying historical inspection logs, food item linkages, and record details. |
| **04. Database Audit & Schema** | [04_database_audit.md](04_database_audit.md) | Direct PostgreSQL database integrity checks, table verification, and key inspection. |

---

## ⚡ Automated Test Runner

You can execute the entire test suite automatically in one command via the automated testing script:

```bash
chmod +x backend/scripts/test_api.sh
./backend/scripts/test_api.sh
```

---

## 🌐 Environment & Base Configuration

- **API Base URL:** `http://localhost:8080/api/v1`
- **Default Server Port:** `8080`
- **Session Cookie Name:** `session` (Encrypted with HMAC-SHA512 + AES-256)
- **Cookie Jar Path:** `.cache_trash/cookies.txt`

