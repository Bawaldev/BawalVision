import http from 'node:http';
import crypto from 'node:crypto';
import { pathToFileURL } from 'node:url';
import { WebSocketServer, WebSocket } from 'ws';

const DEVICE_RE = /^[A-Za-z0-9_.:-]{1,64}$/;

function readConfig(overrides = {}) {
  return {
    port: Number(overrides.port ?? process.env.PORT ?? 8080),
    ingestKey: String(overrides.ingestKey ?? process.env.RELAY_INGEST_KEY ?? ''),
    viewKey: String(overrides.viewKey ?? process.env.RELAY_VIEW_KEY ?? ''),
    defaultDevice: String(overrides.defaultDevice ?? process.env.RELAY_DEFAULT_DEVICE ?? 'default'),
    maxFrameBytes: Number(overrides.maxFrameBytes ?? process.env.RELAY_MAX_FRAME_BYTES ?? 2 * 1024 * 1024),
    watcherBufferLimit: Number(overrides.watcherBufferLimit ?? process.env.RELAY_WATCH_BUFFER_LIMIT ?? 512 * 1024),
    heartbeatMs: Number(overrides.heartbeatMs ?? process.env.RELAY_HEARTBEAT_MS ?? 30000),
    staleMs: Number(overrides.staleMs ?? process.env.RELAY_STALE_MS ?? 5000),
    log: overrides.log ?? ((...args) => console.log(new Date().toISOString(), ...args))
  };
}

function tokenMatches(expected, supplied) {
  if (!expected) return true;
  if (typeof supplied !== 'string') return false;
  const a = Buffer.from(expected);
  const b = Buffer.from(supplied);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

function writeUpgradeError(socket, status, message) {
  const body = JSON.stringify({ error: message });
  const reason = http.STATUS_CODES[status] ?? 'Error';
  socket.write(
    `HTTP/1.1 ${status} ${reason}\r\n` +
    'Content-Type: application/json\r\n' +
    `Content-Length: ${Buffer.byteLength(body)}\r\n` +
    'Connection: close\r\n\r\n' +
    body
  );
  socket.destroy();
}

function jsonResponse(res, status, payload) {
  const body = JSON.stringify(payload);
  res.writeHead(status, {
    'Content-Type': 'application/json',
    'Content-Length': Buffer.byteLength(body),
    'Access-Control-Allow-Origin': '*',
    'Cache-Control': 'no-store'
  });
  res.end(body);
}

export function createRelayServer(overrides = {}) {
  const config = readConfig(overrides);
  const streams = new Map();
  const startedAt = Date.now();

  function getStream(device) {
    let stream = streams.get(device);
    if (!stream) {
      stream = {
        latest: null,
        latestAt: 0,
        ingest: null,
        watchers: new Set(),
        frames: 0,
        dropped: 0,
        fpsStart: 0,
        fpsCount: 0,
        fps: 0
      };
      streams.set(device, stream);
    }
    return stream;
  }

  function notifyWatcherCount(stream) {
    if (stream.ingest && stream.ingest.readyState === WebSocket.OPEN) {
      try {
        stream.ingest.send(JSON.stringify({ type: 'watchers', count: stream.watchers.size }));
      } catch {}
    }
  }

  function broadcast(stream, frame) {
    for (const watcher of stream.watchers) {
      if (watcher.readyState !== WebSocket.OPEN) {
        stream.watchers.delete(watcher);
        continue;
      }
      if (watcher.bufferedAmount > config.watcherBufferLimit) {
        stream.dropped++;
        continue;
      }
      try {
        watcher.send(frame, { binary: true });
      } catch {
        stream.dropped++;
      }
    }
  }

  function streamStats(stream, now) {
    const age = stream.latestAt ? now - stream.latestAt : null;
    return {
      online: age !== null && age <= config.staleMs,
      frame_age_ms: age,
      watchers: stream.watchers.size,
      ingest_connected: Boolean(stream.ingest && stream.ingest.readyState === WebSocket.OPEN),
      fps: Number(stream.fps.toFixed(1)),
      frames: stream.frames,
      dropped: stream.dropped,
      bytes: stream.latest ? stream.latest.length : 0
    };
  }

  const server = http.createServer((req, res) => {
    const url = new URL(req.url ?? '/', `http://${req.headers.host ?? 'localhost'}`);
    if (req.method === 'OPTIONS') {
      res.writeHead(204, {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Methods': 'GET, OPTIONS',
        'Access-Control-Allow-Headers': '*',
        'Access-Control-Max-Age': '86400'
      });
      res.end();
      return;
    }
    if (req.method !== 'GET') {
      jsonResponse(res, 405, { error: 'method not allowed' });
      return;
    }
    const now = Date.now();
    if (url.pathname === '/health' || url.pathname === '/stats') {
      const details = {};
      let online = 0;
      for (const [device, stream] of streams) {
        const stats = streamStats(stream, now);
        if (stats.online) online++;
        details[device] = stats;
      }
      jsonResponse(res, 200, {
        status: 'ok',
        service: 'bawal-camera-relay',
        uptime_s: Math.round((now - startedAt) / 1000),
        streams: streams.size,
        online,
        devices: details
      });
      return;
    }
    if (url.pathname === '/') {
      res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8', 'Access-Control-Allow-Origin': '*' });
      res.end('BawalVision camera relay\nGET /health\nWS /ingest?device=<id>&key=<ingest-key>\nWS /watch?device=<id>\n');
      return;
    }
    jsonResponse(res, 404, { error: 'not found' });
  });

  const ingestWss = new WebSocketServer({
    noServer: true,
    perMessageDeflate: false,
    maxPayload: config.maxFrameBytes + 1024
  });
  const watchWss = new WebSocketServer({
    noServer: true,
    perMessageDeflate: false,
    maxPayload: 64 * 1024
  });

  server.on('upgrade', (req, socket, head) => {
    let url;
    try {
      url = new URL(req.url ?? '/', `http://${req.headers.host ?? 'localhost'}`);
    } catch {
      socket.destroy();
      return;
    }
    const device = (url.searchParams.get('device') || config.defaultDevice).slice(0, 64);
    if (!DEVICE_RE.test(device)) {
      writeUpgradeError(socket, 400, 'invalid device id');
      return;
    }

    if (url.pathname === '/ingest') {
      if (!tokenMatches(config.ingestKey, url.searchParams.get('key') ?? undefined)) {
        config.log(`[relay] rejected ingest for ${device}: bad key`);
        writeUpgradeError(socket, 401, 'invalid ingest key');
        return;
      }
      ingestWss.handleUpgrade(req, socket, head, (ws) => {
        ingestWss.emit('connection', ws, req, { device });
      });
      return;
    }

    if (url.pathname === '/watch') {
      if (!tokenMatches(config.viewKey, url.searchParams.get('key') ?? undefined)) {
        config.log(`[relay] rejected watch for ${device}: bad key`);
        writeUpgradeError(socket, 401, 'invalid view key');
        return;
      }
      watchWss.handleUpgrade(req, socket, head, (ws) => {
        watchWss.emit('connection', ws, req, { device });
      });
      return;
    }

    writeUpgradeError(socket, 404, 'unknown endpoint');
  });

  ingestWss.on('connection', (ws, _req, meta) => {
    const stream = getStream(meta.device);
    if (stream.ingest && stream.ingest.readyState === WebSocket.OPEN) {
      try { stream.ingest.close(1012, 'replaced'); } catch {}
    }
    stream.ingest = ws;
    ws.isAlive = true;
    config.log(`[relay] ingest connected: ${meta.device}`);
    notifyWatcherCount(stream);

    ws.on('pong', () => { ws.isAlive = true; });
    ws.on('message', (data, isBinary) => {
      if (!isBinary) {
        return;
      }
      const frame = Buffer.isBuffer(data) ? data : Buffer.from(data);
      if (frame.length < 4 || frame.length > config.maxFrameBytes) {
        return;
      }
      if (frame[0] !== 0xff || frame[1] !== 0xd8) {
        return;
      }
      const now = Date.now();
      stream.latest = frame;
      stream.latestAt = now;
      stream.frames++;
      if (stream.fpsStart === 0) {
        stream.fpsStart = now;
        stream.fpsCount = 0;
      }
      stream.fpsCount++;
      const elapsed = now - stream.fpsStart;
      if (elapsed >= 1000) {
        stream.fps = (stream.fpsCount * 1000) / elapsed;
        stream.fpsStart = now;
        stream.fpsCount = 0;
      }
      broadcast(stream, frame);
    });
    ws.on('close', () => {
      if (stream.ingest === ws) {
        stream.ingest = null;
        config.log(`[relay] ingest disconnected: ${meta.device}`);
      }
      notifyWatcherCount(stream);
    });
    ws.on('error', (err) => {
      config.log(`[relay] ingest error for ${meta.device}: ${err.message}`);
    });
  });

  watchWss.on('connection', (ws, _req, meta) => {
    const stream = getStream(meta.device);
    stream.watchers.add(ws);
    ws.isAlive = true;
    config.log(`[relay] watcher connected: ${meta.device} (${stream.watchers.size})`);
    notifyWatcherCount(stream);

    ws.on('pong', () => { ws.isAlive = true; });
    ws.on('message', () => {});
    ws.on('close', () => {
      stream.watchers.delete(ws);
      config.log(`[relay] watcher disconnected: ${meta.device} (${stream.watchers.size})`);
      notifyWatcherCount(stream);
    });
    ws.on('error', (err) => {
      config.log(`[relay] watcher error for ${meta.device}: ${err.message}`);
    });

    if (stream.latest) {
      try {
        ws.send(stream.latest, { binary: true });
      } catch {}
    }
  });

  const heartbeat = setInterval(() => {
    for (const wss of [ingestWss, watchWss]) {
      for (const ws of wss.clients) {
        if (ws.isAlive === false) {
          ws.terminate();
          continue;
        }
        ws.isAlive = false;
        try { ws.ping(); } catch {}
      }
    }
  }, config.heartbeatMs);
  heartbeat.unref?.();

  async function close() {
    clearInterval(heartbeat);
    for (const wss of [ingestWss, watchWss]) {
      for (const ws of wss.clients) {
        try { ws.terminate(); } catch {}
      }
      await new Promise((resolve) => wss.close(() => resolve()));
    }
    await new Promise((resolve) => server.close(() => resolve()));
  }

  return { server, config, streams, close };
}

const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
  const { server, config } = createRelayServer();
  server.listen(config.port, '0.0.0.0', () => {
    config.log(`[relay] listening on :${config.port}`);
    if (!config.ingestKey) {
      config.log('[relay] WARNING: RELAY_INGEST_KEY is empty; ingest is open to anyone');
    }
  });
  const shutdown = () => {
    server.close(() => process.exit(0));
    setTimeout(() => process.exit(1), 5000).unref();
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}
