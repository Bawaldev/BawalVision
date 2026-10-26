export const DEFAULT_API_BASE = '/api/v1';

const STORAGE_KEY = 'bawal_backend_url';

export function isValidBackendHost(url: string | null | undefined): boolean {
    if (!url) return false;
    const value = url.trim();
    if (!value || value === DEFAULT_API_BASE || value === `${DEFAULT_API_BASE}/`) return true;
    const cleaned = value.replace(/^https?:\/\//i, '').replace(/[:/].*$/, '').trim();
    if (!cleaned || cleaned === 'localhost' || cleaned === '127.0.0.1') return true;
    if (/^[\d.]+$/.test(cleaned)) {
        const parts = cleaned.split('.');
        return parts.length === 4 && parts.every(p => /^\d+$/.test(p) && Number(p) >= 0 && Number(p) <= 255);
    }
    return /^[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$/.test(cleaned);
}

export function normalizeApiBase(raw: string | null | undefined): string {
    let value = (raw ?? '').trim();
    if (!value || value === DEFAULT_API_BASE || value === `${DEFAULT_API_BASE}/`) {
        return DEFAULT_API_BASE;
    }
    if (!isValidBackendHost(value)) {
        return DEFAULT_API_BASE;
    }
    value = value.replace(/\/+$/, '');
    if (!/^https?:\/\//i.test(value)) {
        value = `http://${value}`;
    }
    value = value.replace(/\/api\/v1$/i, '');
    return `${value}/api/v1`;
}

export function getConfiguredApiBase(): string {
    if (typeof localStorage === 'undefined') {
        return DEFAULT_API_BASE;
    }
    const stored = localStorage.getItem(STORAGE_KEY);
    if (!stored) {
        return DEFAULT_API_BASE;
    }
    // Mixed Content / Private Network Access Guard:
    // When the frontend runs on HTTPS (e.g. Vercel), browsers strictly block plain HTTP requests
    // to loopback or LAN IP addresses (localhost, 127.0.0.1) with CORS / ERR_FAILED.
    if (typeof window !== 'undefined' && window.location.protocol === 'https:') {
        if (/^http:\/\//i.test(stored) || stored.includes('localhost') || stored.includes('127.0.0.1')) {
            return DEFAULT_API_BASE;
        }
    }
    if (!isValidBackendHost(stored)) {
        try { localStorage.removeItem(STORAGE_KEY); } catch {}
        return DEFAULT_API_BASE;
    }
    return normalizeApiBase(stored);
}

export function setConfiguredApiBase(raw: string): string {
    const normalized = normalizeApiBase(raw);
    if (typeof localStorage !== 'undefined') {
        if (normalized === DEFAULT_API_BASE || !isValidBackendHost(normalized)) {
            localStorage.removeItem(STORAGE_KEY);
        } else {
            localStorage.setItem(STORAGE_KEY, normalized);
        }
    }
    return normalized;
}

export function resolveApiBase(baseUrl?: string): string {
    if (baseUrl && baseUrl !== DEFAULT_API_BASE &&
        baseUrl !== `${DEFAULT_API_BASE}/`) {
        if (typeof window !== 'undefined' && window.location.protocol === 'https:') {
            if (/^http:\/\//i.test(baseUrl) || baseUrl.includes('localhost') || baseUrl.includes('127.0.0.1')) {
                return DEFAULT_API_BASE;
            }
        }
        return normalizeApiBase(baseUrl);
    }
    return getConfiguredApiBase();
}

export function resolveApiOrigin(baseUrl?: string): string {
    const base = resolveApiBase(baseUrl);
    return base === DEFAULT_API_BASE ? '' : base.replace(/\/api\/v1$/i, '');
}

export function isLocalBackendConfigured(): boolean {
    return getConfiguredApiBase() !== DEFAULT_API_BASE;
}

async function probeCrowBackend(apiBase: string, timeoutMs: number): Promise<boolean> {
    if (typeof fetch === 'undefined') return false;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
        const response = await fetch(`${apiBase}/health`, {
            signal: controller.signal,
            cache: 'no-store'
        });
        if (!response.ok) return false;
        const payload = await response.json().catch(() => null);
        return payload?.service === 'bawal-crow-backend' &&
            payload?.status === 'ok';
    } catch {
        return false;
    } finally {
        clearTimeout(timer);
    }
}

export async function detectLocalBackend(timeoutMs = 1500): Promise<string | null> {
    if (typeof window === 'undefined') return null;

    // Mixed Content Guard: Browsers strictly block plain HTTP fetches from HTTPS origins
    // (such as bawal-vision.vercel.app or production cloud domains). Probing http:// localhost
    // or LAN IPs from HTTPS triggers browser console security errors and 15-25s hung timeouts.
    const isHttps = window.location.protocol === 'https:';
    if (isHttps) {
        if (await probeCrowBackend(DEFAULT_API_BASE, timeoutMs)) {
            return DEFAULT_API_BASE;
        }
        return null;
    }

    const candidates = new Set<string>();

    // 1. Check if user configured a custom backend in localStorage
    const stored = getConfiguredApiBase();
    if (stored && stored !== DEFAULT_API_BASE) {
        candidates.add(stored);
    }

    // 2. Check stream URL host stored in localStorage (e.g. from camera stream connection)
    try {
        const streamUrl = localStorage.getItem('bawal_esp32_stream_url');
        if (streamUrl) {
            const match = streamUrl.match(/https?:\/\/([^/:]+)/i);
            if (match && match[1]) {
                const candidateHost = match[1].trim();
                const parts = candidateHost.split('.');
                if (parts.length === 4 && parts.every(p => /^\d+$/.test(p) && Number(p) >= 0 && Number(p) <= 255)) {
                    candidates.add(`http://${candidateHost}:8080/api/v1`);
                }
            }
        }
    } catch {}

    // 3. Localhost & loopback endpoints
    candidates.add('http://localhost:8080/api/v1');
    candidates.add('http://127.0.0.1:8080/api/v1');

    const hostname = window.location.hostname;
    if (hostname && hostname !== 'localhost' && hostname !== '127.0.0.1' && !hostname.endsWith('.vercel.app')) {
        candidates.add(`http://${hostname}:8080/api/v1`);
    }

    // Probe local backends first!
    for (const candidate of candidates) {
        if (await probeCrowBackend(candidate, timeoutMs)) {
            setConfiguredApiBase(candidate);
            return normalizeApiBase(candidate);
        }
    }

    // Fallback to cloud default (/api/v1) if no local backend is reachable
    if (await probeCrowBackend(DEFAULT_API_BASE, timeoutMs)) {
        return DEFAULT_API_BASE;
    }

    return null;
}
