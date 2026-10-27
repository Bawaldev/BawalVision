export { RelayRoom } from './relay-room.js';

const DEVICE_RE = /^[A-Za-z0-9_.:-]{1,64}$/;
const encoder = new TextEncoder();

function defaultDevice(env) {
    return String(env.RELAY_DEFAULT_DEVICE || 'default').trim() || 'default';
}

function normalizeDevice(value, env) {
    const raw = String(value || defaultDevice(env)).trim();
    return DEVICE_RE.test(raw) ? raw : '';
}

function tokenMatches(expected, supplied) {
    if (!expected) return true;
    if (typeof supplied !== 'string') return false;
    const a = encoder.encode(expected);
    const b = encoder.encode(supplied);
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff === 0;
}

function json(payload, status = 200) {
    return new Response(JSON.stringify(payload), {
        status,
        headers: {
            'Content-Type': 'application/json',
            'Access-Control-Allow-Origin': '*',
            'Cache-Control': 'no-store'
        }
    });
}

function clientIp(request) {
    // CF-Connecting-IP is authoritative on Workers. The fallback lets local
    // `wrangler dev` / tests exercise the same-network check.
    return String(
        request.headers.get('CF-Connecting-IP') ||
        request.headers.get('x-relay-client-ip') ||
        ''
    ).trim();
}

function withClientIp(request, ip) {
    const headers = new Headers(request.headers);
    if (ip) headers.set('x-relay-client-ip', ip);
    else headers.delete('x-relay-client-ip');
    return new Request(request, { headers });
}

function upgradeError(status, message) {
    return json({ error: message }, status);
}

function corsPreflight() {
    return new Response(null, {
        status: 204,
        headers: {
            'Access-Control-Allow-Origin': '*',
            'Access-Control-Allow-Methods': 'GET, OPTIONS',
            'Access-Control-Allow-Headers': '*',
            'Access-Control-Max-Age': '86400'
        }
    });
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);

        if (request.method === 'OPTIONS') {
            return corsPreflight();
        }

        if (url.pathname === '/' && request.method === 'GET') {
            return new Response(
                'BawalVision camera relay (Cloudflare)\n' +
                'GET /health?device=<id>\n' +
                'WS /ingest?device=<id>&key=<ingest-key>\n' +
                'WS /watch?device=<id>[&key=<view-key>]\n',
                {
                    headers: {
                        'Content-Type': 'text/plain; charset=utf-8',
                        'Access-Control-Allow-Origin': '*'
                    }
                }
            );
        }

        if ((url.pathname === '/health' || url.pathname === '/stats') && request.method === 'GET') {
            const device = normalizeDevice(url.searchParams.get('device'), env);
            if (!device) {
                return json({ error: 'invalid device id' }, 400);
            }
            try {
                const id = env.RELAY.idFromName(device);
                const statsRequest = withClientIp(
                    new Request('https://relay.internal/stats'),
                    clientIp(request)
                );
                const statsRes = await env.RELAY.get(id).fetch(statsRequest);
                const stats = await statsRes.json();
                return json({
                    status: 'ok',
                    service: 'bawal-camera-relay-cf',
                    device,
                    ...stats
                });
            } catch {
                return json({ status: 'error', error: 'relay unavailable' }, 503);
            }
        }

        if (request.method !== 'GET' ||
            (url.pathname !== '/ingest' && url.pathname !== '/watch')) {
            return json({ error: 'not found' }, 404);
        }

        const upgrade = String(request.headers.get('Upgrade') || '').toLowerCase();
        if (upgrade !== 'websocket') {
            return upgradeError(426, 'expected websocket');
        }

        const device = normalizeDevice(url.searchParams.get('device'), env);
        if (!device) {
            return upgradeError(400, 'invalid device id');
        }

        if (url.pathname === '/ingest') {
            if (!tokenMatches(env.RELAY_INGEST_KEY || '', url.searchParams.get('key'))) {
                return upgradeError(401, 'invalid ingest key');
            }
        } else if (!tokenMatches(env.RELAY_VIEW_KEY || '', url.searchParams.get('key'))) {
            return upgradeError(401, 'invalid view key');
        }

        const id = env.RELAY.idFromName(device);
        return env.RELAY.get(id).fetch(withClientIp(request, clientIp(request)));
    }
};
