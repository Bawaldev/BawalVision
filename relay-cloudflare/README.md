# BawalVision Cloudflare camera relay

Serverless WSS relay for ESP32-S3 JPEG frames. One Durable Object per device
keeps only the latest frame in memory and fans it out to browser watchers.

This is the free Cloudflare alternative to the Node/Oracle relay in `../relay/`.
The WebSocket protocol is the same, so the firmware and dashboard work with
either implementation.

## Endpoints

- `GET /health?device=<id>` or `GET /stats?device=<id>` — relay status.
- `WS /ingest?device=<id>&key=<ingest-key>` — ESP32 publisher.
- `WS /watch?device=<id>[&key=<view-key>]` — browser subscriber.

Frames must be binary JPEG starting with `FF D8`. The relay never decodes or
persists frames. A new watcher receives the latest frame immediately when one is
in memory.

## Cloudflare setup

1. Create a Cloudflare account.
2. Open **Workers & Pages** and create a Worker named `bawal-camera-relay`.
3. Register the account-wide `workers.dev` subdomain, for example `fariswkt`.
4. Keep **Protect with Cloudflare Access** off; the ESP32 cannot pass browser SSO.
5. Use the **Workers Free** plan.

The dashboard URL is then:

```text
https://bawal-camera-relay.<subdomain>.workers.dev
```

## Install and run locally

```bash
cd relay-cloudflare
npm install
cp .dev.vars.example .dev.vars
npx wrangler login
npx wrangler whoami
npx wrangler dev
```

`wrangler dev` listens on `http://127.0.0.1:8787`. Set the local secret in
`.dev.vars`:

```dotenv
RELAY_INGEST_KEY=test-ingest-key
# RELAY_VIEW_KEY=test-view-key
```

Run the smoke test in another terminal:

```bash
npm test
```

The test checks a binary frame from ingest to watch, the health endpoint, and a
401 for a bad ingest key. Use a unique device name per run.

## Deploy

```bash
cd relay-cloudflare
npx wrangler secret put RELAY_INGEST_KEY
npx wrangler secret put RELAY_VIEW_KEY
npx wrangler deploy
```

`RELAY_VIEW_KEY` is optional. If it is set, configure the browser with
`PUBLIC_CAMERA_RELAY_VIEW_KEY`; it is visible in the browser bundle and only
deters casual viewers. The ingest key is the real secret.

`RELAY_DEFAULT_DEVICE` is a plain variable in `wrangler.toml` and defaults to
`default`.

After deploy:

```bash
curl "https://bawal-camera-relay.<subdomain>.workers.dev/health?device=default"
npx wrangler tail
```

## ESP32

```dotenv
CAMERA_RELAY_URL=https://bawal-camera-relay.<subdomain>.workers.dev
CAMERA_RELAY_FPS=15
CAMERA_RELAY_DEVICE=default
RELAY_INGEST_KEY=<same key as the Worker secret>
```

The firmware converts `https://` to `wss://`, appends `/ingest`, and adds the
device and key query parameters. `CAMERA_RELAY_FPS=15` keeps the relay inside
the free Durable Objects request budget.

## Dashboard

Set these in Vercel and redeploy:

```dotenv
PUBLIC_CAMERA_RELAY_URL=https://bawal-camera-relay.<subdomain>.workers.dev
PUBLIC_CAMERA_RELAY_DEVICE=default
# PUBLIC_CAMERA_RELAY_VIEW_KEY=<only if RELAY_VIEW_KEY is set>
```

The dashboard converts the base URL to `wss://.../watch`.

## Object detection

The relay only moves JPEG frames; it does not run YOLO. The browser captures the
relayed image every 3 seconds and posts it to `/api/v1/camera/frame`. On Vercel
that path is proxied to the Lambda `/api/v1/lambda/inspect` endpoint, and the
existing `bawal-live-detections` event updates the overlay.

`1 frame / 3 s` is about 864k Lambda requests per month, inside the 1M free
tier. `1 frame / 1.5 s` is about 1.728M and exceeds it. Detection stops when no
dashboard tab is open.

## Free-tier budget

At 15 fps, one device sends about 1.296M WebSocket messages per day. Durable
Objects bill incoming messages at 20:1, or about 64.8k requests/day against the
100k/day free limit. A 24/7 object at 128 MB uses about 11,059 GB-s/day against
the 13,000 GB-s/day free limit. Outgoing WebSocket messages are free.

Do not raise the camera to 20 fps for 24/7 use; that is about 86.4k DO
requests/day and leaves little headroom.

If a daily limit is exceeded, that operation fails until the next UTC reset.
Lower `CAMERA_RELAY_FPS` or use fewer devices.

## Differences from the Node relay

- Health is per device: `/health?device=<id>`. The Node relay reports all
  devices at `/health`.
- Counters and the latest frame live in Durable Object memory and reset on
  eviction/hibernation.
- Watchers that send an application-level `ack` are limited to 3 unacknowledged
  frames; the relay skips frames until the watcher catches up, and closes the
  socket if `send()` fails. The Node relay also checks `bufferedAmount`.
- There is no `/stats` registry across devices.

## Troubleshooting

- **401 on ingest/watch:** the key in the URL does not match the Worker secret.
  Re-run `npx wrangler secret put ...` and update the ESP `.env` or Vercel env.
- **Health says offline:** no frame has arrived within 5 seconds. Check the ESP
  serial log for `Connected to camera relay` and the Worker with
  `npx wrangler tail`.
- **TLS/clock errors on the ESP:** WSS needs SNTP. The firmware rotates
  `pool.ntp.org`, `time.google.com`, and `time.cloudflare.com` every 30 seconds
  while the clock is invalid; if all NTP traffic is blocked, WSS cannot start.
- **No frames in the browser:** confirm `PUBLIC_CAMERA_RELAY_URL` and the device
  id match `CAMERA_RELAY_URL` / `CAMERA_RELAY_DEVICE`.
- **Daily limit errors:** lower `CAMERA_RELAY_FPS` to 12 and monitor the
  Cloudflare dashboard.
- **workers.dev blocked or unavailable:** use a Cloudflare custom domain or the
  Oracle relay in `../relay/`.
