const MAX_FRAME_BYTES = 2 * 1024 * 1024;
const MAX_TELEMETRY_BYTES = 8 * 1024;
const STALE_MS = 5000;
// Keep enough headroom for a browser that acks on the next animation frame;
// three in-flight frames was too aggressive at 10 fps.
const MAX_WATCHER_PENDING = 8;

function isJpegFrame(message) {
    if (!(message instanceof ArrayBuffer)) return false;
    if (message.byteLength < 4 || message.byteLength > MAX_FRAME_BYTES) return false;
    const header = new Uint8Array(message, 0, 2);
    return header[0] === 0xff && header[1] === 0xd8;
}

function normalizeIp(raw) {
    return String(raw || '').trim().toLowerCase().split('%')[0];
}

function expandIpv6(ip) {
    let value = normalizeIp(ip);
    const v4 = value.match(/(\d+\.\d+\.\d+\.\d+)$/);
    if (v4) {
        const parts = v4[1].split('.').map(Number);
        const hi = (((parts[0] << 8) | parts[1]) >>> 0).toString(16);
        const lo = (((parts[2] << 8) | parts[3]) >>> 0).toString(16);
        value = `${value.slice(0, -v4[1].length)}${hi}:${lo}`;
    }
    const [head, tail] = value.split('::');
    const headGroups = head ? head.split(':').filter(Boolean) : [];
    const tailGroups = tail ? tail.split(':').filter(Boolean) : [];
    const missing = Math.max(0, 8 - headGroups.length - tailGroups.length);
    const groups = [
        ...headGroups,
        ...(tail !== undefined ? Array(missing).fill('0') : []),
        ...tailGroups
    ];
    return groups.map((group) => (group || '0').replace(/^0+/, '') || '0');
}

function sameNetwork(a, b) {
    const left = normalizeIp(a);
    const right = normalizeIp(b);
    if (!left || !right) return true;
    if (left === right) return true;
    const leftV6 = left.includes(':');
    const rightV6 = right.includes(':');
    if (leftV6 !== rightV6) return false;
    if (!leftV6) return false;
    // Home ISPs usually delegate a /64; compare the network prefix because
    // IPv6 privacy addresses change per device/tab.
    return expandIpv6(left).slice(0, 4).join(':') ===
        expandIpv6(right).slice(0, 4).join(':');
}

export class RelayRoom {
    constructor(state, env) {
        this.state = state;
        this.env = env;
        this.latest = null;
        this.latestAt = 0;
        this.telemetry = null;
        this.telemetryAt = 0;
        this.frames = 0;
        this.dropped = 0;
        this.fpsStart = 0;
        this.fpsCount = 0;
        this.fps = 0;
        this.ingestIp = '';
        this.restrictViews = String(env.RELAY_ALLOW_REMOTE_VIEW || '') !== '1';
        this.startedAt = Date.now();
    }

    currentIngestIp() {
        if (this.ingestIp) return this.ingestIp;
        for (const socket of this.state.getWebSockets('ingest')) {
            const attachment = socket.deserializeAttachment() || {};
            if (attachment.ip) return attachment.ip;
        }
        return '';
    }

    async fetch(request) {
        const url = new URL(request.url);
        if (url.pathname.endsWith('/stats')) {
            const requestIp = String(request.headers.get('x-relay-client-ip') || '').trim();
            let includeTelemetry = true;
            if (this.restrictViews) {
                const expected = this.currentIngestIp() ||
                    String(await this.state.storage.get('ingestIp') || '');
                if (expected && requestIp) {
                    includeTelemetry = sameNetwork(expected, requestIp);
                }
            }
            return Response.json(this.stats(includeTelemetry));
        }

        const upgrade = String(request.headers.get('Upgrade') || '').toLowerCase();
        if (upgrade !== 'websocket') {
            return new Response('expected websocket', { status: 426 });
        }

        const role = url.pathname.endsWith('/ingest')
            ? 'ingest'
            : url.pathname.endsWith('/watch')
                ? 'watch'
                : '';
        if (!role) {
            return new Response('not found', { status: 404 });
        }

        const requestIp = String(request.headers.get('x-relay-client-ip') || '').trim();

        if (role === 'watch' && this.restrictViews) {
            const expected = this.currentIngestIp() ||
                String(await this.state.storage.get('ingestIp') || '');
            if (expected && requestIp && !sameNetwork(expected, requestIp)) {
                return new Response(JSON.stringify({
                    error: 'watch requires the same network as the camera'
                }), {
                    status: 403,
                    headers: { 'Content-Type': 'application/json' }
                });
            }
        }

        if (role === 'ingest') {
            this.ingestIp = requestIp;
            if (requestIp) {
                const stored = String(await this.state.storage.get('ingestIp') || '');
                if (stored !== requestIp) {
                    await this.state.storage.put('ingestIp', requestIp);
                }
            }
            for (const existing of this.state.getWebSockets('ingest')) {
                try {
                    existing.close(1012, 'replaced');
                } catch {}
            }
            if (this.restrictViews && requestIp) {
                for (const watcher of this.state.getWebSockets('watch')) {
                    const attachment = watcher.deserializeAttachment() || {};
                    if (attachment.ip && !sameNetwork(requestIp, attachment.ip)) {
                        try { watcher.close(1008, 'network changed'); } catch {}
                    }
                }
            }
        }

        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair);
        this.state.acceptWebSocket(server, [role]);

        if (role === 'watch') {
            server.serializeAttachment({ pending: 0, ackCapable: false, ip: requestIp });
            if (this.latest) {
                try {
                    server.send(this.latest);
                } catch {}
            }
            if (this.telemetry) {
                try {
                    server.send(this.telemetry);
                } catch {}
            }
        }

        if (role === 'ingest') {
            server.serializeAttachment({ ip: requestIp });
            try {
                server.send(JSON.stringify({
                    type: 'watchers',
                    count: this.state.getWebSockets('watch').length
                }));
            } catch {}
        }

        return new Response(null, { status: 101, webSocket: client });
    }

    async webSocketMessage(ws, message) {
        const tags = this.state.getTags(ws);
        if (tags.includes('watch')) {
            if (typeof message === 'string' && message === 'ack') {
                const state = ws.deserializeAttachment() || {};
                state.ackCapable = true;
                state.pending = Math.max(0, Number(state.pending || 0) - 1);
                ws.serializeAttachment(state);
            }
            return;
        }
        if (!tags.includes('ingest')) return;
        if (typeof message === 'string') {
            this.handleTelemetry(message);
            return;
        }
        if (!isJpegFrame(message)) return;

        const now = Date.now();
        this.latest = message;
        this.latestAt = now;
        this.frames++;
        if (this.fpsStart === 0) {
            this.fpsStart = now;
            this.fpsCount = 0;
        }
        this.fpsCount++;
        const elapsed = now - this.fpsStart;
        if (elapsed >= 1000) {
            this.fps = (this.fpsCount * 1000) / elapsed;
            this.fpsStart = now;
            this.fpsCount = 0;
        }

        const ingestIp = this.currentIngestIp();

        for (const watcher of this.state.getWebSockets('watch')) {
            const state = watcher.deserializeAttachment() || {};
            if (this.restrictViews && ingestIp && state.ip &&
                !sameNetwork(ingestIp, state.ip)) {
                try { watcher.close(1008, 'network changed'); } catch {}
                continue;
            }
            if (state.ackCapable && Number(state.pending || 0) >= MAX_WATCHER_PENDING) {
                this.dropped++;
                continue;
            }
            state.pending = Number(state.pending || 0) + 1;
            watcher.serializeAttachment(state);
            try {
                watcher.send(message);
            } catch {
                this.dropped++;
                try {
                    watcher.close(1011, 'send failed');
                } catch {}
            }
        }
    }

    handleTelemetry(message) {
        if (message.length > MAX_TELEMETRY_BYTES) return;
        let parsed;
        try {
            parsed = JSON.parse(message);
        } catch {
            return;
        }
        if (!parsed || parsed.type !== 'telemetry') return;
        this.telemetry = message;
        this.telemetryAt = Date.now();
        const ingestIp = this.currentIngestIp();
        for (const watcher of this.state.getWebSockets('watch')) {
            const state = watcher.deserializeAttachment() || {};
            if (this.restrictViews && ingestIp && state.ip &&
                !sameNetwork(ingestIp, state.ip)) {
                try { watcher.close(1008, 'network changed'); } catch {}
                continue;
            }
            try {
                watcher.send(message);
            } catch {}
        }
    }

    async webSocketClose(ws, code, reason, _wasClean) {
        const role = this.state.getTags(ws).join(',') || 'unknown';
        console.log('relay socket close', role, code, String(reason || ''));
    }

    async webSocketError(ws, error) {
        const role = this.state.getTags(ws).join(',') || 'unknown';
        console.log('relay socket error', role, String(error && error.message || error));
    }

    stats(includeTelemetry = true) {
        const now = Date.now();
        const age = this.latestAt ? now - this.latestAt : null;
        const online = age !== null && age <= STALE_MS;
        return {
            online,
            frame_age_ms: age,
            watchers: this.state.getWebSockets('watch').length,
            ingest_connected: this.state.getWebSockets('ingest').length > 0,
            fps: online ? Number(this.fps.toFixed(1)) : 0,
            frames: this.frames,
            dropped: this.dropped,
            bytes: this.latest ? this.latest.byteLength : 0,
            view_restricted: this.restrictViews,
            telemetry: includeTelemetry && this.telemetry ? JSON.parse(this.telemetry) : null,
            telemetry_age_ms: includeTelemetry && this.telemetryAt ? now - this.telemetryAt : null,
            uptime_s: Math.round((now - this.startedAt) / 1000)
        };
    }
}
