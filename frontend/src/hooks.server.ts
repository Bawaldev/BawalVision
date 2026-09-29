import type { Handle } from '@sveltejs/kit';
import { env } from '$env/dynamic/private';

export const handle: Handle = async ({ event, resolve }) => {
    if (event.url.pathname.startsWith('/api/')) {
        const backendUrl = env.BACKEND_URL || (globalThis as any).process?.env?.BACKEND_URL || 'http://backend:8080';
        const target = `${backendUrl}${event.url.pathname}${event.url.search}`;

        try {
            const res = await fetch(target, {
                method: event.request.method,
                headers: event.request.headers,
                body: ['GET', 'HEAD'].includes(event.request.method) ? undefined : await event.request.arrayBuffer()
            });

            const headers = new Headers(res.headers);
            headers.delete('content-encoding');

            return new Response(res.body, {
                status: res.status,
                statusText: res.statusText,
                headers
            });
        } catch (err) {
            console.error(`[PROXY ERROR] Failed to proxy to ${target}:`, err);
            return new Response(JSON.stringify({ error: 'Backend unreachable' }), {
                status: 502,
                headers: { 'Content-Type': 'application/json' }
            });
        }
    }

    return resolve(event);
};
