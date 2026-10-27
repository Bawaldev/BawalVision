const MAX_FRAME_BYTES = 2 * 1024 * 1024;
const STALE_MS = 5000;
const MAX_WATCHER_PENDING = 3;

function isJpegFrame(message) {
    if (!(message instanceof ArrayBuffer)) return false;
    if (message.byteLength < 4 || message.byteLength > MAX_FRAME_BYTES) return false;
    const header = new Uint8Array(message, 0, 2);
    return header[0] === 0xff && header[1] === 0xd8;
}

export class RelayRoom {
    constructor(state, env) {
        this.state = state;
        this.env = env;
        this.latest = null;
        this.latestAt = 0;
        this.frames = 0;
        this.dropped = 0;
        this.fpsStart = 0;
        this.fpsCount = 0;
        this.fps = 0;
        this.startedAt = Date.now();
    }

    async fetch(request) {
        const url = new URL(request.url);
        if (url.pathname.endsWith('/stats')) {
            return Response.json(this.stats());
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

        if (role === 'ingest') {
            for (const existing of this.state.getWebSockets('ingest')) {
                try {
                    existing.close(1012, 'replaced');
                } catch {}
            }
        }

        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair);
        this.state.acceptWebSocket(server, [role]);

        if (role === 'watch') {
            server.serializeAttachment({ pending: 0, ackCapable: false });
            if (this.latest) {
                try {
                    server.send(this.latest);
                } catch {}
            }
        }

        if (role === 'ingest') {
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

        for (const watcher of this.state.getWebSockets('watch')) {
            const state = watcher.deserializeAttachment() || {};
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

    async webSocketClose(ws, code, reason, _wasClean) {
        const role = this.state.getTags(ws).join(',') || 'unknown';
        console.log('relay socket close', role, code, String(reason || ''));
    }

    async webSocketError(ws, error) {
        const role = this.state.getTags(ws).join(',') || 'unknown';
        console.log('relay socket error', role, String(error && error.message || error));
    }

    stats() {
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
            uptime_s: Math.round((now - this.startedAt) / 1000)
        };
    }
}
