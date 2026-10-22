## 📌 Pull Request Overview
<!-- Provide a concise summary of changes and the problem being solved. -->

### Subsystem Affected:
- [ ] `services/` (C++20 Crow Backend, Machine Vision & Inference Pipeline)
- [ ] `edge/` (ESP32-S3 Firmware & Sensor Drivers)
- [ ] `frontend/` (SvelteKit 5 Dashboard & Telemetry UI)
- [ ] `ml/` (Dataset Scripts & Neural Network Models)
- [ ] `deploy/` / `.github/` (Docker, Infrastructure, CI/CD)

---

## 🔍 Validation Checklist
Please verify the following before submitting this pull request:

- [ ] **No Secrets or Credentials Committed**: Verified that `.env`, Wi-Fi credentials (`wifi_credentials.h`), AWS keys, or private certificates are NOT tracked or committed.
- [ ] **No Oversized Binaries or Raw Datasets**: Confirmed no `.onnx`, `.pth`, `.bin`, `.tar.gz`, or raw image dataset files are committed to the Git index.
- [ ] **C++ Vision Compilation**: Passed local build (`just build-vision`) with no `-march=native` non-portable compiler flags.
- [ ] **Backend Go Tests & Race Detector**: Verified passing tests via `go test -race ./...` (or `just test-backend`).
- [ ] **Firmware Verification** (if edge modified): Built cleanly with ESP-IDF (`just build-edge`).
- [ ] **Frontend Verification** (if frontend modified): Type-check passed via `npm run check` (or `just check-frontend`).
- [ ] **Documentation & Change Log**: Updated `OUTPUT.md` / `README.md` if user-facing behavior, API, or architectural changes were introduced.

---

## 🧪 Testing Performed
<!-- Describe the tests, hardware verification, or simulation steps taken to test your changes. -->
- Hardware / Board tested (e.g. ESP32-S3-WROOM-1, OV5640, ST7789 TFT):
- Host Environment tested (e.g. Linux x86_64, macOS ARM64, Docker):
- Test commands run:

---

## 🔗 Related Issues
<!-- Closes #123, Fixes #456 -->
Fixes #

