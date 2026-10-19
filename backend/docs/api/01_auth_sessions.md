# Module 01: Open Web Access & Zero-Login Architecture

BawalVision operates as an **open-source web application** designed for transparent, friction-free local and edge deployment.

### Key Architectural Characteristics:
1. **Zero Login / Authentication Screen:** No authentication wall, registration forms, or login prompts. The inspection dashboard, camera feeds, and telemetry logs are directly accessible upon launching.
2. **Zero Cookie Persistence:** No session cookies, tracking cookies, or auth cookies are stored in client browsers or backend databases.
3. **Open Inspection & Telemetry Endpoints:** All REST endpoints (`/api/v1/records`, `/api/v1/telemetry`, `/api/v1/inspections`, `/api/v1/camera/latest`, etc.) are directly queryable by edge devices and dashboard clients without session overhead.
