const RELAY_VIEW_KEY_STORAGE = 'bawal_relay_view_key';

/**
 * Read the relay view key from localStorage (preferred) or a one-time
 * `?view_key=`/`?key=` URL parameter. The key is deliberately not read from
 * PUBLIC_* env vars: those are bundled into the browser and cannot be secret.
 */
export function getRelayViewKey(): string {
    if (typeof localStorage !== 'undefined') {
        try {
            const stored = (localStorage.getItem(RELAY_VIEW_KEY_STORAGE) || '').trim();
            if (stored) return stored;
        } catch {}
    }
    if (typeof window !== 'undefined') {
        try {
            const params = new URLSearchParams(window.location.search);
            const fromUrl = (params.get('view_key') || params.get('key') || '').trim();
            if (fromUrl) {
                setRelayViewKey(fromUrl);
                return fromUrl;
            }
        } catch {}
    }
    return '';
}

export function setRelayViewKey(key: string): void {
    if (typeof localStorage === 'undefined') return;
    const value = (key || '').trim();
    try {
        if (value) localStorage.setItem(RELAY_VIEW_KEY_STORAGE, value);
        else localStorage.removeItem(RELAY_VIEW_KEY_STORAGE);
    } catch {}
}

export function clearRelayViewKey(): void {
    setRelayViewKey('');
}

export function promptRelayViewKey(): string {
    if (typeof window === 'undefined') return '';
    const entered = window.prompt(
        'Enter the relay view key.\n\n' +
        'It is the value set with: npx wrangler secret put RELAY_VIEW_KEY\n' +
        'Leave blank to keep the current same-network access mode.',
        ''
    );
    if (entered === null) return '';
    const value = entered.trim();
    if (value) setRelayViewKey(value);
    return value;
}
