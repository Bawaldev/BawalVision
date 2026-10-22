# AGENTS.md

Polyglot monorepo (OpenCV AI Competition 2026). No root build system besides `just`;
recipes live in `Justfile` + `scripts/{backend,edge,ml}.just` (those files are the
source of truth — README tables can lag behind).

## Layout & entrypoints

- `edge/` — C / FreeRTOS ESP-IDF firmware for ESP32-S3. Entry `edge/main/src/main.c`.
- `services/` — C++20 backend + vision pipeline (Crow, OpenCV, ONNX Runtime, libpq,
  OpenMP). Primary entry `services/src/backend/main_backend.cpp` → binary
  `services/build/bin/bawal_backend`. Legacy standalone viewer remains at
  `services/src/main.cpp` → `services/build/bin/vision_service`. API docs/curl
  suite in `docs/api/` and `deploy/test_api.sh`.
- `frontend/` — SvelteKit 5 + Vite, `frontend/src/routes`. Typecheck = `svelte-check`,
  **no test suite**.
- `ml/` — Python training/quantization pipeline, runs in root `.venv`.
- `deploy/` — `docker-compose.yml` + Dockerfiles (compose file is NOT at repo root).
- `.github/CODEOWNERS` maps subsystem ownership; `.github/workflows/` CI is
  path-scoped per subsystem.

## Commands

- `just --list` — all recipes.
- Vision (C++): `just build-vision` (auto-configures CMake into `services/build`),
  `just run-vision`, `just run-vision-tsan` (gdb + ThreadSanitizer + `services/tsan_suppressions.txt`).
- Backend (C++ Crow): `just build-backend`, `just run-backend`,
  `just test-api` (needs a live backend + `examples/`), or the full Docker stack with
  `just compose-up`.
- Frontend: `just install-frontend`, `just check-frontend` (the typecheck), `just run-frontend`.
- Edge: `just build-edge` / `just run-edge [port]` — requires ESP-IDF; recipes source
  `$IDF_PATH/export.sh` (default `$HOME/esp/esp-idf`), `idf.py` is not on PATH.
- ML: `just setup-ml` (installs CUDA torch from `--extra-index-url`, heavy), then
  `just pipeline-ai1` end-to-end; see `ml/README.md`.
- Stack: `just compose-up [service]` / `just compose-build` / `just compose-logs`.

## Verify before a PR (mirrors CI)

1. `just build-vision` — CI builds gcc-14 and clang-18 and greps `services/` for
   `-march=native`; it is forbidden. Use explicit flags (`-mavx2`, runtime `__AVX2__`
   checks, NEON for ARM).
2. `just compose-up` then `just test-api` — verifies the C++ Crow backend, SHM frame
   ingress, in-process YOLO, and PostgreSQL persistence.
3. `just check-frontend`.

Commits follow Conventional Commits with scope: `feat(vision):`, `fix(backend):`,
`perf(edge):`, `docs(ml):`.

## Gotchas (things you would likely get wrong)

- **Postgres is optional at startup for the C++ backend.** It logs a warning and keeps
  serving in-memory snapshots when the DB is unreachable. Defaults are
  `DB_HOST=postgres` (Docker DNS), `DB_PORT=5432`; for a host-side run use
  `just run-backend` (starts compose Postgres and sets `DB_HOST=localhost DB_PORT=5433`).
- **Schema lives in `deploy/postgres/shema.sql`** (misspelled on purpose) and is only the
  compose initdb seed. The C++ backend does not run migrations; append `ALTER TABLE
  ... IF NOT EXISTS` blocks there for schema changes, and recreate the volume (or apply
  them manually) when upgrading an existing `postgres_data` volume.
- **No `-march=native`, ever** (CI enforces). Same for arch portability claims in
  `README`/`CONTRIBUTING`.
- **ONNX Runtime is not installed by `just setup-deps`** (apt packages only). CMake
  looks in `/opt/onnxruntime` then `/usr/local`; CI installs v1.19.2 and passes
  `-DONNXRUNTIME_INCLUDE_DIR=/usr/local/include -DONNXRUNTIME_LIB=/usr/local/lib/libonnxruntime.so`.
- **`bawal_backend` is the server**: ESP32 frames arrive at `POST /api/v1/camera/frame`,
  go through POSIX SHM to the in-process vision thread, and results are served from
  `/api/v1/inspections/latest`. The legacy `vision_service` binary is still a client
  (pulls `/api/v1/camera/latest/...`, POSTs results) and supports
  `VIDEO_PATH=/path/file.mp4` (device id `MOCK_VIDEO_CAM`); it opens an OpenCV GUI
  window unless `HEADLESS=1`.
- **Docker Desktop for Linux cannot serve LAN TCP payloads through published ports**
  (TCP connect succeeds, HTTP requests never reach the container). The compose stack
  publishes the C++ backend on `0.0.0.0:8080`; on affected Docker Desktop setups use
  `just run-backend` (host process) and point the ESP32 at the laptop IP.
- **lwIP TCP buffers cap at 65535** (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT` /
  `CONFIG_LWIP_TCP_WND_DEFAULT`). `65536` is outside the Kconfig range when
  `LWIP_WND_SCALE=off`, so Kconfig silently falls back to 5760. Check the generated
  `edge/build/config/sdkconfig.h`, not just `sdkconfig.defaults`.
- **Host-side USB auto-provisioning is opt-in** (`USB_AUTO_PROVISION=1`). It otherwise
  opens `/dev/ttyACM*` every 4 s and conflicts with `just run-edge`/`idf.py monitor`,
  and can reset the board. USB status/provision HTTP endpoints still work on demand.
- **Postgres host port is `${POSTGRES_PORT:-5433} -> 5432`** (not 5433 -> 5433);
  the in-container port is always 5432.
- **Model selection**: `MODEL_PATH`, else the search list in
  `services/src/backend/vision_worker.cpp` (`ml/AI1/model/yolov8s_int8/...` first, then
  `/app/...` and `weights/`). `weights/` and `*.pt` are gitignored — a fresh clone has
  no trained weights.
- **Ports**: C++ backend reads `CROW_PORT` (default `8080`) and is published on
  `0.0.0.0:8080`; frontend Vite dev `:5173` (proxies `/api` →
  `BACKEND_URL` or `127.0.0.1:8080`); postgres `${POSTGRES_PORT:-5433}` → container
  `5432` (in-network `DB_PORT=5432` works; host-side `DB_PORT=5433`). The compose file
  only runs `postgres` and `bawal_backend`.
- **`.env` is loaded by `just`** (`set dotenv-load := true`) **and baked into the edge
  firmware** as compile definitions at CMake configure time
  (`edge/main/CMakeLists.txt`, `WIFI_SSID`/`WIFI_PASS`/`BACKEND_IP`/`BACKEND_PORT`).
  `.env` is gitignored; edit `.env.example` for new keys. Changing `.env` re-triggers
  edge configure automatically.
- **Compile commands & clangd**: For clangd, `.clangd` reads the per-tree
  databases `edge/build/compile_commands.json` and
  `services/build/compile_commands.json`; generate them with `just build-edge` and
  `just configure-vision`. MS IntelliSense is disabled (`.vscode/settings.json`).
- **Frontend lockfiles are gitignored** (`package-lock.json`), so `npm ci` can never
  work; use `just install-frontend` (`npm install`). `.npmrc` sets
  `engine-strict=true` and `legacy-peer-deps=true`.
- **Never commit**: `.env*`, raw dataset images (`ml/dataset`, `ml/AI1/dataset`),
  `weights/`, `*.pt`, files >25MB. CI (`.github/workflows/security.yml`) runs gitleaks
  and hygiene checks — warnings only, not blockers.
- **Temporary/build artifacts belong in `.cache_trash/`** (gitignored), not the repo
  root. `OUTPUT.md` is a gitignored local agent log.

## Existing instruction sources

- `GEMINI.md` — Gemini CLI protocol; the parts that matter beyond Gemini: log
  agentic/automated changes (scope, rationale) for competition disclosure, keep
  secrets out of reports/commits, route temp files to `.cache_trash/`.
- `.github/CONTRIBUTING.md` — commit conventions, portability rule, local checks.
- `README.md` — architecture diagram, hardware pinout, full `just` cheatsheet.
- `ml/README.md` — stage-by-stage ML pipeline; `docs/api/` — endpoint suites.
