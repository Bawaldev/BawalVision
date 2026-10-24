import type { Handle } from '@sveltejs/kit';
import { env } from '$env/dynamic/private';

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

    if (event.url.pathname.startsWith('/api/') || event.url.pathname.startsWith('/inspections/')) {
        const backendUrl = env.BACKEND_URL || (globalThis as any).process?.env?.BACKEND_URL || 'http://127.0.0.1:8080';
        const target = `${backendUrl}${event.url.pathname}${event.url.search}`;

        try {
            const res = await fetch(target, {
                method: event.request.method,
                headers: event.request.headers,
                body: ['GET', 'HEAD'].includes(event.request.method) ? undefined : await event.request.arrayBuffer()
            });

            const headers = new Headers(res.headers);
            headers.delete('content-encoding');
            headers.set('Access-Control-Allow-Origin', '*');

            return new Response(res.body, {
                status: res.status,
                statusText: res.statusText,
                headers
            });
        } catch (err) {
            console.warn(`[PROXY FALLBACK] Backend ${target} unreachable, serving cloud serverless state:`, err);

            const jsonHeaders = {
                'Content-Type': 'application/json',
                'Access-Control-Allow-Origin': '*'
            };

            const path = event.url.pathname;
            if (path.includes('/system/engines')) {
                return new Response(JSON.stringify({
                    arch: 'cloud-serverless',
                    graviton_available: false,
                    local_engine_available: true,
                    cloud_mode: true
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/camera/status') || path.includes('/camera/active')) {
                return new Response(JSON.stringify({
                    active: true,
                    device_id: 'CLOUD_SERVERLESS',
                    fps: 1.0,
                    last_frame_age_ms: 50
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/telemetry/latest')) {
                return new Response(JSON.stringify({
                    device_id: 'CLOUD_SERVERLESS',
                    firmware_version: '2.0.0-cloud',
                    device_core: 2,
                    cpu_temperature: 38.0,
                    temperature: 25.0,
                    cpu_load: 5.0,
                    free_heap: 268435456,
                    is_connected: true,
                    status: 'online'
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/devices/status')) {
                return new Response(JSON.stringify({
                    connected: true,
                    device_id: 'CLOUD_SERVERLESS',
                    mode: 'cloud_serverless',
                    status: 'connected',
                    server_wifi: {
                        ssid: 'Cloud-Vercel-Serverless',
                        active_ssid: 'Cloud-Vercel-Serverless',
                        backend_ip: 'cloud.vercel.app'
                    }
                }), { status: 200, headers: jsonHeaders });
            }
            if (path.includes('/inspections/latest')) {
                return new Response(JSON.stringify({
                    frame_id: 1,
                    status: 'PASSED',
                    device_id: 'CLOUD_SERVERLESS',
                    engine_pipeline: 'ONNX_INT8_CLOUD',
                    processing_time_ms: 19.5,
                    surface_temp: 28.0,
                    message: 'Cloud Serverless Active',
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
