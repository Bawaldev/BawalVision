# AGENTS.md

Polyglot monorepo (OpenCV AI Competition 2026). No root build system besides `just`;
recipes live in `Justfile` + `scripts/{backend,edge,ml}.just` (those files are the
source of truth — README tables can lag behind).

## Layout & entrypoints

- `edge/` — C / FreeRTOS ESP-IDF firmware for ESP32-S3. Entry `edge/main/src/main.c`.
- `services/` — C++20 vision service (OpenCV, ONNX Runtime, OpenMP). Entry
  `services/src/main.cpp` → binary `services/build/bin/vision_service`.
- `backend/` — Go module `bawal_vision_backend` (Fiber v2, GORM/Postgres). Entry
  `backend/cmd/server/main.go`. API docs/curl suites in `backend/docs/api/`.
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
- Backend (Go): `just test-backend` (`go test -v ./...`; verified to pass with **no**
  database), `just build-gateway`, `just run-gateway`, `just test-api` (needs a live
  backend + `examples/`).
- Frontend: `just install-frontend`, `just check-frontend` (the typecheck), `just run-frontend`.
- Edge: `just build-edge` / `just run-edge [port]` — requires ESP-IDF; recipes source
  `$IDF_PATH/export.sh` (default `$HOME/esp/esp-idf`), `idf.py` is not on PATH.
- ML: `just setup-ml` (installs CUDA torch from `--extra-index-url`, heavy), then
  `just pipeline-ai1` end-to-end; see `ml/README.md`.
- Stack: `just compose-up [service]` / `just compose-build` / `just compose-logs`.

## Verify before a PR (mirrors CI)

1. `just test-backend` — CI additionally runs `go test -race -v ./...`.
2. golangci-lint — **only in CI**: `golangci-lint run --timeout=5m` from `backend/`,
   v1.64.6, default linters (no config file in repo).
3. `just build-vision` — CI builds gcc-14 and clang-18 and greps `services/` for
   `-march=native`; it is forbidden. Use explicit flags (`-mavx2`, runtime `__AVX2__`
   checks, NEON for ARM).
4. `just check-frontend`.

Commits follow Conventional Commits with scope: `feat(vision):`, `fix(backend):`,
`perf(edge):`, `docs(ml):`.

## Gotchas (things you would likely get wrong)

- **Backend exits at startup if Postgres is unreachable.** Defaults are
  `DB_HOST=postgres` (Docker DNS), `DB_PORT=5432`. For a local run use
  `just edge-stack` (compose Postgres + host gateway) or set
  `DB_HOST=localhost DB_PORT=5433`.
- **Migrations are inline**: GORM `AutoMigrate` + ad-hoc `ALTER TABLE`/`DO $$` blocks
  in `backend/cmd/server/main.go` run on every boot. `backend/internal/data/templates/shema.sql`
  (misspelled on purpose) is only the compose initdb seed — editing it alone changes nothing
  for existing volumes.
- **No `-march=native`, ever** (CI enforces). Same for arch portability claims in
  `README`/`CONTRIBUTING`.
- **ONNX Runtime is not installed by `just setup-deps`** (apt packages only). CMake
  looks in `/opt/onnxruntime` then `/usr/local`; CI installs v1.19.2 and passes
  `-DONNXRUNTIME_INCLUDE_DIR=/usr/local/include -DONNXRUNTIME_LIB=/usr/local/lib/libonnxruntime.so`.
- **Vision service is a client, not a server**: it pulls frames from the Go backend
  (`/api/v1/camera/latest/...`) and POSTs results back. Without a backend, run with
  `VIDEO_PATH=/path/file.mp4` (device id becomes `MOCK_VIDEO_CAM`). It opens an OpenCV
  GUI window unless `HEADLESS=1`.
- **Docker Desktop for Linux cannot serve LAN TCP payloads through published ports**
  (TCP connect succeeds, HTTP requests never reach the container; UDP discovery on
  8765 does work). The compose backend is published on `127.0.0.1:${BACKEND_HOST_PORT:-18080}`;
  run `just edge-proxy` to bridge `:8080 -> 18080` for ESP32-S3, or use
  `just edge-stack` (Postgres in compose, backend on the host).
- **Host-side USB auto-provisioning is opt-in** (`USB_AUTO_PROVISION=1`). It otherwise
  opens `/dev/ttyACM*` every 4 s and conflicts with `just run-edge`/`idf.py monitor`,
  and can reset the board. USB status/provision HTTP endpoints still work on demand.
- **Postgres host port is `${POSTGRES_PORT:-5433} -> 5432`** (not 5433 -> 5433);
  the in-container port is always 5432.
- **Model selection**: `MODEL_PATH`, else a search list in `services/src/main.cpp`
  (`ml/AI1/model/yolov8s_int8/...` first, then `weights/`). `weights/` and `*.pt` are
  gitignored — a fresh clone has no trained weights.
- **Ports**: backend `:8080` (hardcoded `app.Listen(":8080")`; `BACKEND_PORT` is only
  read for firewall/URLs), compose publishes the backend on
  `127.0.0.1:${BACKEND_HOST_PORT:-18080}`, frontend Vite dev `:5173` (proxies `/api` →
  `BACKEND_URL` or `127.0.0.1:8080`), compose maps frontend `3001→3000`,
  vision `8081→8080`, postgres `${POSTGRES_PORT:-5433}` → container `5432`
  (in-network `DB_PORT=5432` works; host-side `DB_PORT=5433`).
- **`.env` is loaded by `just`** (`set dotenv-load := true`) **and baked into the edge
  firmware** as compile definitions at CMake configure time
  (`edge/main/CMakeLists.txt`, `WIFI_SSID`/`WIFI_PASS`/`BACKEND_IP`/`BACKEND_PORT`).
  `.env` is gitignored; edit `.env.example` for new keys. Changing `.env` re-triggers
  edge configure automatically.
- **`just configure-all` / `just merge-compile-commands` are broken** — they call
  `scripts/combine_compile_commands.py`, which does not exist. For clangd, `.clangd`
  reads the per-tree databases `edge/build/compile_commands.json` and
  `services/build/compile_commands.json`; generate them with `just build-edge` and
  `just configure-vision`. MS IntelliSense is disabled (`.vscode/settings.json`).
- **Frontend lockfiles are gitignored** (`package-lock.json`), so `npm ci` can never
  work; use `just install-frontend` (`npm install`). `.npmrc` sets
  `engine-strict=true` and `legacy-peer-deps=true`.
- **Go `go.mod` says `go 1.22`; CI pins 1.24.**
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
- `ml/README.md` — stage-by-stage ML pipeline; `backend/docs/api/` — endpoint suites.
