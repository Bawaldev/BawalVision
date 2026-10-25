import type { Handle } from '@sveltejs/kit';
import { env } from '$env/dynamic/private';

let warnedMissingLambdaToken = false;
let warnedApiGatewaySigV4 = false;

export const handle: Handle = async ({ event, resolve }) => {
    if (event.request.method === 'OPTIONS') {
        return new Response(null, {
            status: 204,
            headers: {
                'Access-Control-Allow-Origin': '*',
                'Access-Control-Allow-Methods': 'GET, POST, PUT, DELETE, OPTIONS, HEAD',
                'Access-Control-Allow-Headers': '*'
            }
        });
    }

    if (event.url.pathname.startsWith('/api/v1/examples/file/')) {
        const filePath = event.url.pathname.replace('/api/v1/examples/file/', '/examples/');
        return new Response(null, {
            status: 302,
            headers: { Location: filePath }
        });
    }

    if (event.url.pathname.startsWith('/api/') || event.url.pathname.startsWith('/inspections/')) {
        const backendUrl = env.BACKEND_URL || (globalThis as any).process?.env?.BACKEND_URL || 'http://127.0.0.1:8080';
        const lambdaAuthToken = env.BAWAL_LAMBDA_AUTH_TOKEN ||
            (globalThis as any).process?.env?.BAWAL_LAMBDA_AUTH_TOKEN || '';
        const isLambdaBackend = backendUrl.includes('execute-api') || backendUrl.includes('lambda-url');
        if (isLambdaBackend && !lambdaAuthToken && !warnedMissingLambdaToken) {
            warnedMissingLambdaToken = true;
            console.warn('[PROXY] BAWAL_LAMBDA_AUTH_TOKEN is not configured; Lambda HTTP invocations will be rejected.');
        }
        if (backendUrl.includes('execute-api') && !warnedApiGatewaySigV4) {
            warnedApiGatewaySigV4 = true;
            console.warn('[PROXY] API Gateway is configured with AWS_IAM, but this proxy does not sign requests with SigV4. Use the Lambda Function URL with BAWAL_LAMBDA_AUTH_TOKEN instead.');
        }
        let targetPath = event.url.pathname;
        if (isLambdaBackend) {
            if (targetPath === '/api/v1/camera/frame') {
                targetPath = '/api/v1/lambda/inspect';
            }
        }
        const target = `${backendUrl}${targetPath}${event.url.search}`;

        const jsonHeaders = {
            'Content-Type': 'application/json',
            'Access-Control-Allow-Origin': '*'
        };

        const proxyHeaders = new Headers(event.request.headers);
        proxyHeaders.delete('host');
        proxyHeaders.delete('content-length');
        if (lambdaAuthToken) {
            proxyHeaders.set('x-bawal-token', lambdaAuthToken);
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

        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 3500);

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

            const headers = new Headers(res.headers);
            headers.delete('content-encoding');
            headers.set('Access-Control-Allow-Origin', '*');

            return new Response(res.body, {
                status: res.status,
                statusText: res.statusText,
                headers
            });
        } catch (err) {
            clearTimeout(timeoutId);
            console.warn(`[PROXY FALLBACK] Backend ${target} unreachable, serving cloud serverless state:`, err);

            const path = event.url.pathname;
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
            if (path.includes('/records') || path.includes('/inspections') || path.includes('/examples')) {
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
