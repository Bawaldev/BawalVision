# BawalVision camera relay

Stateful WebSocket relay that lets the ESP32-S3 publish camera frames from any
network and lets the HTTPS dashboard subscribe without mixed-content or
Private Network Access blocks.

```
ESP32-S3  --WSS /ingest-->  relay  --WSS /watch-->  browser
```

The relay keeps only the latest JPEG per device and drops frames for slow
watchers, so latency stays low instead of building a backlog.

## Protocol

| Endpoint | Direction | Payload |
| --- | --- | --- |
| `wss://<host>/ingest?device=<id>&key=<ingest-key>` | ESP32 → relay | binary JPEG frames |
| `wss://<host>/watch?device=<id>&key=<view-key>` | relay → browser | binary JPEG frames |
| `GET /health` | anyone | JSON status, FPS, frame age, watchers |
| `GET /stats` | anyone | alias for `/health` |

`device` is optional and defaults to `RELAY_DEFAULT_DEVICE` (`default`). Text
messages sent by the relay to the ingest socket are JSON control messages
(`{"type":"watchers","count":1}`); the ESP32 may ignore them.

## Environment

| Variable | Default | Purpose |
| --- | --- | --- |
| `PORT` | `8080` | HTTP/WS listen port |
| `RELAY_INGEST_KEY` | empty | Shared key required from the ESP32. Empty disables auth (dev only). |
| `RELAY_VIEW_KEY` | empty | Optional key required from browsers. |
| `RELAY_DEFAULT_DEVICE` | `default` | Stream id used when `device` is omitted. |
| `RELAY_MAX_FRAME_BYTES` | `2097152` | Reject larger ingest frames. |
| `RELAY_WATCH_BUFFER_LIMIT` | `524288` | Drop frames for watchers whose socket buffer exceeds this. |
| `RELAY_STALE_MS` | `5000` | `/health` marks a stream offline after this gap. |

## Run locally

```bash
cd relay
npm install
RELAY_INGEST_KEY=dev-secret node server.js
# then open http://127.0.0.1:8080/health
```

Set the ESP32 `.env` to the base URL (`ws://<laptop-ip>:8080`) and the dashboard
to `PUBLIC_CAMERA_RELAY_URL=http://<laptop-ip>:8080` (the browser upgrades it to
`ws://`).

## Container image

The image lives in `deploy/Dockerfile.relay` and uses the repository root as
its build context (the other Dockerfiles in this repo follow the same
convention). It exposes port `8080` and a `/health` check.

```bash
# from the repo root
docker build -f deploy/Dockerfile.relay -t bawal-camera-relay .
docker run --rm -p 8080:8080 -e RELAY_INGEST_KEY=dev-secret bawal-camera-relay
```

Or run `just build-relay`.

## Deploy on Oracle Cloud Always Free

The reference deployment is a single Oracle Cloud Always Free VM running
`deploy/docker-compose.relay.yml`. Caddy terminates TLS with a Let's Encrypt
certificate and proxies WSS to the relay; only ports 80/443 are public.

### 1. Create the VM

Oracle Console → Compute → Instances → Create Instance:

- Image: **Ubuntu 24.04**
- Shape: **VM.Standard.A1.Flex** (1 OCPU / 1 GB is enough) or the Always Free
  **VM.Standard.E2.1.Micro**
- Assign a **public IPv4 address**
- Save the public IP; the default 50 GB boot volume fits the Always Free quota

### 2. Open the VCN ports

VCN → Security Lists → Default Security List → Add Ingress Rules:

| Source | Protocol | Destination port |
| --- | --- | --- |
| `0.0.0.0/0` | TCP | `80` |
| `0.0.0.0/0` | TCP | `443` |

Keep SSH (22) restricted to your own IP. Do **not** publish `8080`.

### 3. Open the OS firewall

Oracle's Ubuntu images have an iptables REJECT rule in front of the VCN rules;
open 80/443 on the host too:

```bash
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y iptables-persistent
sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 80 -j ACCEPT
sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 443 -j ACCEPT
sudo netfilter-persistent save
```

### 4. Install Docker

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker "$USER"
newgrp docker
```

### 5. Clone and configure

```bash
git clone https://github.com/Bawaldev/BawalVision.git
cd BawalVision
cp .env.example .env
```

Edit `.env`:

```dotenv
# Use a real domain, DuckDNS, or the free wildcard-DNS form:
#   relay.<PUBLIC_IP_WITH_DOTS>.nip.io
RELAY_DOMAIN = relay.<public-ip>.nip.io

RELAY_INGEST_KEY = <openssl rand -hex 24>
CAMERA_RELAY_DEVICE = default
```

Generate the key and `nip.io` hostname directly if you prefer:

```bash
KEY=$(openssl rand -hex 24)
PUBLIC_IP=$(curl -s https://api.ipify.org)
sed -i -E "s|^RELAY_INGEST_KEY[[:space:]]*=.*|RELAY_INGEST_KEY = $KEY|" .env
sed -i -E "s|^RELAY_DOMAIN[[:space:]]*=.*|RELAY_DOMAIN = relay.${PUBLIC_IP}.nip.io|" .env
```

Keep this key: the ESP32 firmware and the relay must use the same value.

### 6. Start the stack

From the repository root:

```bash
docker compose -f deploy/docker-compose.relay.yml --env-file .env up -d --build
docker compose -f deploy/docker-compose.relay.yml ps
docker compose -f deploy/docker-compose.relay.yml logs -f caddy
```

Caddy obtains the certificate on first start. The domain must resolve to the
VM's public IP and port 80 must be reachable for the ACME HTTP challenge.

### 7. Verify

```bash
curl -s "https://$RELAY_DOMAIN/health" | python3 -m json.tool
```

Before the ESP32 connects, `"online": 0`. After it connects:

```json
{
  "online": 1,
  "devices": {
    "default": {
      "online": true,
      "ingest_connected": true,
      "fps": 15.0
    }
  }
}
```

### 8. Point the ESP32 and dashboard at it

Use `https://$RELAY_DOMAIN` for:

- `CAMERA_RELAY_URL` (ESP32 `.env`; firmware converts `https://` to `wss://`)
- `PUBLIC_CAMERA_RELAY_URL` (Vercel env for the dashboard)
- `PUBLIC_CAMERA_RELAY_VIEW_KEY` if `RELAY_VIEW_KEY` is set

Then rebuild/flash the firmware and redeploy Vercel.

### Updating

```bash
git pull
docker compose -f deploy/docker-compose.relay.yml --env-file .env up -d --build
```

### Oracle gotchas

- Both the **VCN security list** and the VM's **iptables** must allow 80/443.
- `VM.Standard.A1.Flex` capacity is region-dependent; retry later or use the
  AMD micro shape. The relay is light enough for 1 GB of RAM.
- `nip.io`/`sslip.io` are convenient for demos; use a real domain for anything
  long-lived.
- The relay port 8080 stays internal to the compose network. Do not expose it.
- Caddy needs outbound HTTPS to Let's Encrypt and inbound port 80 for issuance.
- `RELAY_INGEST_KEY` is a secret. Never prefix it with `PUBLIC_`; only the URL
  and device id are browser-visible.
