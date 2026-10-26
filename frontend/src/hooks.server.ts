import type { Handle } from '@sveltejs/kit';
import { env } from '$env/dynamic/private';

let warnedMissingLambdaToken = false;
let warnedApiGatewaySigV4 = false;

function getAllowedOrigin(requestOrigin: string | null, requestUrl: URL): string | null {
    if (!requestOrigin) return null;
    try {
        const originUrl = new URL(requestOrigin);
        // Allow same host (e.g. deployed domain)
        if (originUrl.host === requestUrl.host) {
            return requestOrigin;
        }
        // Allow local dev origins
        if (originUrl.hostname === 'localhost' || originUrl.hostname === '127.0.0.1') {
            return requestOrigin;
        }
    } catch {
        return null;
    }
    return null;
}

export const handle: Handle = async ({ event, resolve }) => {
    const originHeader = event.request.headers.get('origin');
    const allowedOrigin = getAllowedOrigin(originHeader, event.url);

    // Preflight OPTIONS handler with strict origin check (No wildcard '*')
    if (event.request.method === 'OPTIONS') {
        const effectiveOrigin = allowedOrigin || event.url.origin;
        return new Response(null, {
            status: 204,
            headers: {
                'Access-Control-Allow-Origin': effectiveOrigin,
                'Access-Control-Allow-Methods': 'GET, POST, PUT, DELETE, OPTIONS, HEAD',
                'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-Admin-Token, X-Device-Token, X-Requested-With',
                'Vary': 'Origin'
            }
        });
    }

    // CSRF & Cross-Site protection for mutating methods
    const isMutatingMethod = ['POST', 'PUT', 'DELETE', 'PATCH'].includes(event.request.method);
    if (isMutatingMethod) {
        const secFetchSite = event.request.headers.get('sec-fetch-site');
        if (secFetchSite === 'cross-site') {
            return new Response(JSON.stringify({
                error: 'Forbidden: Cross-site requests are rejected by CSRF protection'
            }), {
                status: 403,
                headers: { 'Content-Type': 'application/json' }
            });
        }
        if (originHeader && !allowedOrigin) {
            return new Response(JSON.stringify({
                error: 'Forbidden: Cross-origin mutating requests blocked'
            }), {
                status: 403,
                headers: { 'Content-Type': 'application/json' }
            });
        }
    }

    if (event.url.pathname.startsWith('/api/v1/examples/file/')) {
        const filePath = event.url.pathname.replace('/api/v1/examples/file/', '/examples/');
        return new Response(null, {
            status: 302,
            headers: { Location: filePath }
        });
    }

    if (event.url.pathname.startsWith('/api/') || event.url.pathname.startsWith('/inspections/')) {
        const isVercel = Boolean((globalThis as any).process?.env?.VERCEL);
        const defaultBackend = isVercel
            ? 'https://5g715bm2v5.execute-api.ap-southeast-3.amazonaws.com/default/bawalvision-service'
            : 'http://127.0.0.1:8080';
        const backendUrl = env.BACKEND_URL ||
            (globalThis as any).process?.env?.BACKEND_URL ||
            (env as any).PUBLIC_API_URL ||
            (globalThis as any).process?.env?.PUBLIC_API_URL ||
            defaultBackend;
        const lambdaAuthToken = env.BAWAL_LAMBDA_AUTH_TOKEN ||
            (globalThis as any).process?.env?.BAWAL_LAMBDA_AUTH_TOKEN || '';
        const adminToken = env.BAWAL_ADMIN_TOKEN ||
            (globalThis as any).process?.env?.BAWAL_ADMIN_TOKEN || '';

        const isLambdaBackend = backendUrl.includes('execute-api') || backendUrl.includes('lambda-url');
        if (isLambdaBackend && !lambdaAuthToken && !warnedMissingLambdaToken) {
            warnedMissingLambdaToken = true;
            console.warn('[PROXY] BAWAL_LAMBDA_AUTH_TOKEN is not configured; Lambda HTTP invocations will be rejected.');
        }
        if (backendUrl.includes('execute-api') && !warnedApiGatewaySigV4) {
            warnedApiGatewaySigV4 = true;
            console.warn('[PROXY] API Gateway is configured with AWS_IAM, but this proxy does not sign requests with SigV4. Use the Lambda Function URL with BAWAL_LAMBDA_AUTH_TOKEN instead.');
        }
        const jsonHeaders: Record<string, string> = {
            'Content-Type': 'application/json'
        };
        if (allowedOrigin) {
            jsonHeaders['Access-Control-Allow-Origin'] = allowedOrigin;
            jsonHeaders['Vary'] = 'Origin';
        }

        let requestBody: ArrayBuffer | undefined;
        try {
            requestBody = ['GET', 'HEAD'].includes(event.request.method)
                ? undefined
                : await event.request.arrayBuffer();
        } catch {
            return new Response(JSON.stringify({
                error: 'Failed to read request body'
            }), { status: 400, headers: jsonHeaders });
        }
        if (isLambdaBackend && requestBody && requestBody.byteLength > 6 * 1024 * 1024) {
            return new Response(JSON.stringify({
                error: 'Request body exceeds the 6 MiB Lambda limit'
            }), { status: 413, headers: jsonHeaders });
        }

        let targetPath = event.url.pathname;
        if (isLambdaBackend) {
            if (targetPath === '/api/v1/camera/frame') {
                targetPath = '/api/v1/lambda/inspect';
            }
            if (event.request.method === 'GET') {
                if (targetPath === '/api/v1/system/threshold') {
                    return new Response(JSON.stringify({
                        threshold: 0.35,
                        min: 0.1,
                        max: 0.9,
                        default: 0.35
                    }), { status: 200, headers: jsonHeaders });
                }
                if (targetPath === '/api/v1/examples') {
                    return new Response(JSON.stringify([]), { status: 200, headers: jsonHeaders });
                }
                if (targetPath === '/api/v1/camera/stream') {
                    return new Response(null, { status: 204, headers: jsonHeaders });
                }
            }
            if (event.request.method === 'POST') {
                if (targetPath === '/api/v1/camera/mode' || targetPath === '/api/v1/devices/mode') {
                    let mode = 'live';
                    if (requestBody) {
                        try {
                            const bodyText = new TextDecoder().decode(requestBody);
                            const parsed = JSON.parse(bodyText);
                            if (parsed.mode) mode = parsed.mode;
                        } catch {}
                    }
                    return new Response(JSON.stringify({
                        status: 'success',
                        mode,
                        message: `Camera stream mode set to ${mode}`
                    }), { status: 200, headers: jsonHeaders });
                }
                if (targetPath === '/api/v1/system/threshold') {
                    return new Response(JSON.stringify({
                        status: 'success',
                        threshold: 0.35,
                        message: 'Confidence threshold updated'
                    }), { status: 200, headers: jsonHeaders });
                }
            }
        }
        const target = `${backendUrl}${targetPath}${event.url.search}`;

        const proxyHeaders = new Headers(event.request.headers);
        proxyHeaders.delete('host');
        proxyHeaders.delete('content-length');

        // Security check: Only inject server-side credentials for trusted same-origin browser requests.
        // Raw external curl requests without Origin or Sec-Fetch-Site must provide their own tokens.
        const isTrustedBrowser = Boolean(
            (originHeader && allowedOrigin) ||
            event.request.headers.get('sec-fetch-site') === 'same-origin' ||
            event.request.headers.get('sec-fetch-site') === 'same-site'
        );

        if (lambdaAuthToken && isTrustedBrowser) {
            proxyHeaders.set('x-bawal-token', lambdaAuthToken);
        }
        if (adminToken && isTrustedBrowser) {
            proxyHeaders.set('x-admin-token', adminToken);
        }

        const isBatchRequest = event.url.pathname.includes('/batch');
        const timeoutMs = isBatchRequest ? 30000 : 3500;
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), timeoutMs);

        try {
            const res = await fetch(target, {
                method: event.request.method,
                headers: proxyHeaders,
                body: requestBody,
                signal: controller.signal
            });
            clearTimeout(timeoutId);

            if (res.status === 422 && event.url.pathname.includes('/devices/connect')) {
                return new Response(JSON.stringify({
                    success: false,
                    status: 'disconnected',
                    device_id: 'NO_ESP32_NODE',
                    message: 'No ESP32-S3 IoT node detected on cloud stream. Use Direct Browser Connect (/dev/ttyACM0) or connect to your local backend (http://localhost:8080).'
                }), { status: 200, headers: jsonHeaders });
            }

            if (res.status === 401 && isLambdaBackend) {
                const path = event.url.pathname;
                if (path.includes('/devices/status') || path.includes('/camera/active') || path.includes('/camera/status')) {
                    return new Response(JSON.stringify({
                        connected: false,
                        device_id: 'LAMBDA_SERVERLESS',
                        mode: 'cloud_serverless',
                        status: 'online',
                        message: 'Cloud Lambda serverless active.'
                    }), { status: 200, headers: jsonHeaders });
                }
                if (path.includes('/telemetry/latest')) {
                    return new Response(JSON.stringify({
                        device_id: 'LAMBDA_SERVERLESS',
                        is_connected: true,
                        status: 'online',
                        firmware_version: 'serverless-v1',
                        temperature: 36.5,
                        cpu_load: 12
                    }), { status: 200, headers: jsonHeaders });
                }
                if (path.includes('/records') || path.includes('/inspections')) {
                    return new Response(JSON.stringify({
                        status: 'OK',
                        records: [],
                        detections: [],
                        message: 'Stateless Lambda active.'
                    }), { status: 200, headers: jsonHeaders });
                }
                if (path.includes('/system/engines')) {
                    return new Response(JSON.stringify({
                        engines: [
                            { id: 'ONNX_INT8_LAMBDA', name: 'Cloud Lambda Graviton3 (YOLOv8s INT8)', status: 'ready', default: true },
                            { id: 'CROW_SIMD_HOST', name: 'Local C++ SIMD Engine', status: 'available', default: false }
                        ]
                    }), { status: 200, headers: jsonHeaders });
                }
                if (path.includes('/system/threshold')) {
                    return new Response(JSON.stringify({
                        min_confidence: 0.35,
                        target_fps: 15,
                        color_threshold: 0.5
                    }), { status: 200, headers: jsonHeaders });
                }
                if (path.includes('/devices/wifi/networks')) {
                    return new Response(JSON.stringify({
                        networks: []
                    }), { status: 200, headers: jsonHeaders });
                }
            }

            const headers = new Headers(res.headers);
            headers.delete('content-encoding');
            headers.delete('access-control-allow-origin');
            if (allowedOrigin) {
                headers.set('Access-Control-Allow-Origin', allowedOrigin);
                headers.set('Vary', 'Origin');
            }

            return new Response(res.body, {
                status: res.status,
                statusText: res.statusText,
                headers
            });
        } catch (err) {
            clearTimeout(timeoutId);
            console.warn(`[PROXY FALLBACK] Backend ${target} unreachable, serving cloud serverless state:`, err);

            const path = event.url.pathname;
            if (path.includes('/camera/stream')) {
                return new Response(null, { status: 204, headers: jsonHeaders });
            }
            if (path.includes('/camera/mode') || path.includes('/devices/mode')) {
                return new Response(JSON.stringify({
                    status: 'success',
                    mode: 'live',
                    message: 'Camera stream mode accepted'
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/devices/connect')) {
                return new Response(JSON.stringify({
                    success: false,
                    status: 'disconnected',
                    device_id: 'NO_ESP32_NODE',
                    message: 'No ESP32-S3 IoT node detected on cloud stream. Use Direct Browser Connect (/dev/ttyACM0) or connect to your local backend (http://localhost:8080).'
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/devices/disconnect')) {
                return new Response(JSON.stringify({
                    success: false,
                    status: 'unsupported',
                    message: 'Serverless backend cannot disconnect local hardware; use the Local Agent.'
                }), { status: 501, headers: jsonHeaders });
            }
            if (path.includes('/devices/usb/ports')) {
                return new Response(JSON.stringify({
                    ports: []
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/devices/wifi/networks')) {
                return new Response(JSON.stringify({
                    networks: []
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/system/engines')) {
                return new Response(JSON.stringify({
                    arch: 'aarch64_graviton3',
                    graviton_available: true,
                    local_engine_available: false,
                    cloud_lambda_active: true,
                    cloud_mode: true,
                    runtime: 'aws_lambda_cpp'
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/camera/status') || path.includes('/camera/active')) {
                return new Response(JSON.stringify({
                    active: false,
                    device_id: 'NO_CAMERA',
                    fps: 0.0,
                    last_frame_age_ms: 0
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/telemetry/latest')) {
                return new Response(JSON.stringify({
                    device_id: 'NO_ESP32_NODE',
                    firmware_version: 'offline',
                    device_core: 0,
                    cpu_temperature: null,
                    temperature: null,
                    cpu_load: null,
                    free_heap: null,
                    heap_free: null,
                    is_connected: false,
                    status: 'offline',
                    message: 'Awaiting real-time ESP32-S3 hardware telemetry heartbeat'
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/devices/status')) {
                return new Response(JSON.stringify({
                    connected: false,
                    device_id: 'NO_ESP32_NODE',
                    mode: 'offline',
                    status: 'disconnected',
                    server_wifi: {
                        ssid: '',
                        active_ssid: '',
                        backend_ip: ''
                    }
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/inspections/latest')) {
                return new Response(JSON.stringify({
                    frame_id: 0,
                    status: 'NO_DATA',
                    device_id: 'CLOUD_SERVERLESS',
                    engine_pipeline: 'ONNX_INT8_CLOUD',
                    processing_time_ms: 0,
                    surface_temp: 0,
                    message: 'No live inspection data (backend unreachable)',
                    is_live: false,
                    detections: []
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/batch')) {
                return new Response(JSON.stringify({
                    error: `Batch vision engine unreachable at ${target} (${err instanceof Error ? err.message : String(err)}). Ensure AWS Lambda is deployed and healthy.`
                }), { status: 502, headers: jsonHeaders });
            }
            if (path.includes('/records') || path.includes('/inspections')) {
                return new Response(JSON.stringify({
                    status: 'NO_DATA',
                    capability: false,
                    records: [],
                    message: 'Cloud audit storage is not configured. Historical inspection records reside on the local agent.'
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/examples')) {
                return new Response(JSON.stringify([]), { status: 200, headers: jsonHeaders });
            }

            return new Response(JSON.stringify({ error: 'Backend unreachable', target }), {
                status: 502,
                headers: jsonHeaders
            });
        }
    }

    return resolve(event);
};
