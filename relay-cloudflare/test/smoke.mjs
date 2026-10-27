import assert from 'node:assert/strict';
import WebSocket from 'ws';

const rawBase = (process.env.RELAY_URL || 'ws://127.0.0.1:8787').replace(/\/+$/, '');
const wsBase = rawBase.replace(/^http:/i, 'ws:').replace(/^https:/i, 'wss:');
const httpBase = wsBase.replace(/^ws:/i, 'http:').replace(/^wss:/i, 'https:');
const device = `smoke-${Date.now().toString(36)}`;
const key = process.env.RELAY_INGEST_KEY || 'test-ingest-key';
const viewKey = process.env.RELAY_VIEW_KEY || '';
// Miniflare/wrangler dev preserves this test header. Real Cloudflare deploys
// always use the unspoofable CF-Connecting-IP, so only run the network test locally.
const localDev = /^(127\.0\.0\.1|localhost)$/.test(new URL(httpBase).hostname);
const testIp = localDev ? '203.0.113.10' : '';
const otherIp = localDev ? '198.51.100.20' : '';
const frame = new Uint8Array([
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a,
    0x46, 0x49, 0x46, 0x00, 0x01, 0xff, 0xd9
]);

function connect(url, expectedStatus = 0, ip = testIp) {
    return new Promise((resolve, reject) => {
        const headers = ip
            ? { 'CF-Connecting-IP': ip, 'x-relay-client-ip': ip }
            : undefined;
        const ws = new WebSocket(url, { headers });
        let settled = false;
        const timer = setTimeout(() => {
            try { ws.terminate(); } catch {}
            finish(reject, new Error(`timeout connecting to ${url}`));
        }, 8000);

        function finish(fn, value) {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            fn(value);
        }

        ws.once('open', () => finish(resolve, ws));
        ws.once('error', (err) => finish(reject, err));
        ws.once('unexpected-response', (_req, res) => {
            res.resume();
            if (expectedStatus && res.statusCode === expectedStatus) {
                finish(resolve, null);
            } else {
                finish(reject, new Error(`unexpected HTTP ${res.statusCode} from ${url}`));
            }
        });
    });
}

function nextBinary(ws, timeoutMs = 8000) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('timeout waiting for frame')), timeoutMs);
        ws.on('message', function onMessage(data, isBinary) {
            if (!isBinary) return;
            clearTimeout(timer);
            ws.off('message', onMessage);
            resolve({ data, isBinary });
        });
    });
}

function nextTelemetry(ws, timeoutMs = 8000) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('timeout waiting for telemetry')), timeoutMs);
        ws.on('message', function onMessage(data, isBinary) {
            if (isBinary) return;
            clearTimeout(timer);
            ws.off('message', onMessage);
            resolve(JSON.parse(data.toString()));
        });
    });
}

const watchUrl = `${wsBase}/watch?device=${encodeURIComponent(device)}${viewKey ? `&key=${encodeURIComponent(viewKey)}` : ''}`;
const ingestUrl = `${wsBase}/ingest?device=${encodeURIComponent(device)}&key=${encodeURIComponent(key)}`;
const badKeyUrl = `${wsBase}/ingest?device=${encodeURIComponent(device)}&key=wrong-key`;
const healthUrl = `${httpBase}/health?device=${encodeURIComponent(device)}`;

let watch;
let ingest;
let badKey;

try {
    watch = await connect(watchUrl);
    const received = nextBinary(watch);

    ingest = await connect(ingestUrl);
    ingest.send(frame);

    const message = await received;
    assert.equal(message.isBinary, true);
    assert.deepEqual(new Uint8Array(message.data), frame);
    watch.send('ack');

    const telemetry = {
        type: 'telemetry',
        device_id: 'smoke-device',
        cpu_temperature: 41.5,
        cpu_load: 12,
        free_heap: 123456
    };
    const telemetryReceived = nextTelemetry(watch);
    ingest.send(JSON.stringify(telemetry));
    assert.deepEqual(await telemetryReceived, telemetry);

    if (localDev) {
        const outsider = await connect(watchUrl, 403, otherIp);
        assert.equal(outsider, null);
    }

    const healthRes = await fetch(healthUrl);
    assert.equal(healthRes.status, 200);
    const health = await healthRes.json();
    assert.equal(health.status, 'ok');
    assert.equal(health.online, true);
    assert.equal(health.ingest_connected, true);
    assert.equal(health.watchers, 1);
    assert.ok(health.frames >= 1);
    assert.equal(health.bytes, frame.byteLength);
    assert.equal(health.telemetry.cpu_temperature, 41.5);
    assert.ok(health.telemetry_age_ms != null);

    badKey = await connect(badKeyUrl, 401);
    assert.equal(badKey, null);

    console.log(`smoke ok: ${healthUrl}`);
} finally {
    try { badKey?.close(); } catch {}
    try { watch?.close(); } catch {}
    try { ingest?.close(); } catch {}
}

process.exit(0);
