export const DEFAULT_API_BASE = '/api/v1';

const STORAGE_KEY = 'bawal_backend_url';

export function normalizeApiBase(raw: string | null | undefined): string {
    let value = (raw ?? '').trim();
    if (!value || value === DEFAULT_API_BASE || value === `${DEFAULT_API_BASE}/`) {
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
    return normalizeApiBase(localStorage.getItem(STORAGE_KEY));
}

export function setConfiguredApiBase(raw: string): string {
    const normalized = normalizeApiBase(raw);
    if (typeof localStorage !== 'undefined') {
        if (normalized === DEFAULT_API_BASE) {
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

export async function detectLocalBackend(timeoutMs = 1200): Promise<string | null> {
    if (typeof window === 'undefined') return null;

    if (await probeCrowBackend(DEFAULT_API_BASE, timeoutMs)) {
        return null;
    }

    const candidates = new Set<string>();
    const stored = getConfiguredApiBase();
    if (stored !== DEFAULT_API_BASE) {
        candidates.add(stored);
    }
    candidates.add('http://localhost:8080/api/v1');
    candidates.add('http://127.0.0.1:8080/api/v1');

    const hostname = window.location.hostname;
    if (hostname && hostname !== 'localhost' && hostname !== '127.0.0.1') {
        candidates.add(`http://${hostname}:8080/api/v1`);
    }

    for (const candidate of candidates) {
        if (await probeCrowBackend(candidate, timeoutMs)) {
            setConfiguredApiBase(candidate);
            return normalizeApiBase(candidate);
        }
    }
    return null;
}
