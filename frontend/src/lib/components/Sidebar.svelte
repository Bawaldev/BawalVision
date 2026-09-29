<script lang="ts">
    interface Props {
        activeTab: string;
        onSelectTab: (tab: string) => void;
        activeDevice?: string;
        isOnline?: boolean;
    }

    let { activeTab = 'Dashboard', onSelectTab, activeDevice = 'ESP32S3_441BF6C95678', isOnline = true }: Props = $props();

    const menuItems = [
        { id: 'Dashboard', label: 'Dashboard', icon: 'M4 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2V6zM14 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2V6zM4 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2v-2zM14 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2v-2z' },
        { id: 'Telemetry Analytics', label: 'Telemetry Analytics', icon: 'M3 13.125C3 12.504 3.504 12 4.125 12h2.25c.621 0 1.125.504 1.125 1.125v6.75C7.5 20.496 6.996 21 6.375 21h-2.25A1.125 1.125 0 013 19.875v-6.75zM9.75 8.625c0-.621.504-1.125 1.125-1.125h2.25c.621 0 1.125.504 1.125 1.125v11.25c0 .621-.504 1.125-1.125 1.125h-2.25a1.125 1.125 0 01-1.125-1.125V8.625zM16.5 4.125c0-.621.504-1.125 1.125-1.125h2.25C20.496 3 21 3.504 21 4.125v15.75c0 .621-.504 1.125-1.125 1.125h-2.25a1.125 1.125 0 01-1.125-1.125V4.125z' },
        { id: 'Result Log', label: 'Result Log', icon: 'M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z' },
        { id: 'System & Cloud Settings', label: 'System & Cloud Settings', icon: 'M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z M15 12a3 3 0 11-6 0 3 3 0 016 0z' }
    ];
</script>

<aside class="sidebar-container">
    <!-- Brand Header -->
    <div class="brand-box">
        <div class="brand-emblem">
            <svg viewBox="0 0 40 40" class="logo-svg">
                <circle cx="20" cy="20" r="18" fill="none" stroke="currentColor" stroke-width="2.5" />
                <line x1="7" y1="7" x2="33" y2="33" stroke="currentColor" stroke-width="2.2" />
                <line x1="7" y1="33" x2="33" y2="7" stroke="currentColor" stroke-width="2.2" />
                <circle cx="20" cy="20" r="6" fill="currentColor" fill-opacity="0.3" />
            </svg>
        </div>
        <div class="brand-text">
            <h2>Bawal</h2>
            <span>Vision</span>
            <small class="version-tag">AI INSPECTION 2026</small>
        </div>
    </div>

    <!-- Navigation Menu -->
    <nav class="nav-menu">
        {#each menuItems as item}
            <button
                type="button"
                class="nav-btn"
                class:active={activeTab === item.id}
                onclick={() => onSelectTab(item.id)}
            >
                <svg class="nav-svg" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                    <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d={item.icon}></path>
                </svg>
                <span class="nav-text">{item.label}</span>
                {#if item.id === 'Dashboard'}
                    <span class="live-pill">LIVE</span>
                {/if}
            </button>
        {/each}
    </nav>

    <!-- Sidebar Bottom System Status -->
    <div class="sidebar-status-box">
        <div class="device-badge">
            <div class="status-indicator" class:online={isOnline}></div>
            <div class="device-meta">
                <span class="device-label">Edge Node</span>
                <strong class="device-id-val">{activeDevice.slice(0, 14)}</strong>
            </div>
        </div>
        <div class="platform-meta">
            <span>OpenCV AI 2026</span>
            <span>v1.0.4-prod</span>
        </div>
    </div>
</aside>

<style>
    .sidebar-container {
        width: 260px;
        background: var(--bg-sidebar, #121824);
        border-right: 1px solid var(--border-color, #1f293d);
        display: flex;
        flex-direction: column;
        flex-shrink: 0;
        user-select: none;
    }

    .brand-box {
        display: flex;
        align-items: center;
        gap: 14px;
        padding: 22px 18px;
        border-bottom: 1px solid var(--border-color, #1f293d);
        background: rgba(0, 0, 0, 0.15);
    }

    .brand-emblem {
        width: 44px;
        height: 44px;
        color: var(--accent-cyan, #00f0ff);
        display: flex;
        align-items: center;
        justify-content: center;
        filter: drop-shadow(0 0 8px rgba(0, 240, 255, 0.4));
    }

    .logo-svg {
        width: 100%;
        height: 100%;
    }

    .brand-text h2 {
        font-size: 1.15rem;
        font-weight: 800;
        letter-spacing: -0.02em;
        margin: 0;
        color: #ffffff;
        line-height: 1.1;
    }

    .brand-text span {
        font-size: 1.1rem;
        font-weight: 700;
        color: var(--accent-amber, #ffb703);
        text-transform: uppercase;
        letter-spacing: 0.05em;
    }

    .version-tag {
        display: block;
        font-size: 0.65rem;
        color: #64748b;
        letter-spacing: 0.1em;
        margin-top: 3px;
    }

    .nav-menu {
        display: flex;
        flex-direction: column;
        gap: 8px;
        padding: 20px 14px;
        flex: 1;
    }

    .nav-btn {
        display: flex;
        align-items: center;
        gap: 12px;
        width: 100%;
        padding: 13px 14px;
        background: transparent;
        border: 1px solid transparent;
        border-radius: 8px;
        color: #94a3b8;
        font-size: 0.9rem;
        font-weight: 600;
        text-align: left;
        cursor: pointer;
        transition: all 0.2s ease;
    }

    .nav-btn:hover {
        background: rgba(255, 255, 255, 0.04);
        color: #f1f5f9;
        border-color: rgba(255, 255, 255, 0.08);
    }

    .nav-btn.active {
        background: var(--active-gradient, linear-gradient(90deg, rgba(0, 240, 255, 0.15), rgba(0, 240, 255, 0.02)));
        color: #ffffff;
        border-color: rgba(0, 240, 255, 0.35);
        box-shadow: 0 0 15px rgba(0, 240, 255, 0.1);
    }

    .nav-svg {
        width: 20px;
        height: 20px;
        color: inherit;
        flex-shrink: 0;
    }

    .nav-btn.active .nav-svg {
        color: var(--accent-cyan, #00f0ff);
    }

    .nav-text {
        flex: 1;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }

    .live-pill {
        font-size: 0.65rem;
        padding: 2px 6px;
        background: rgba(16, 185, 129, 0.2);
        color: #10b981;
        border: 1px solid rgba(16, 185, 129, 0.4);
        border-radius: 4px;
        font-weight: 700;
        letter-spacing: 0.05em;
        animation: pulse 2s infinite;
    }

    @keyframes pulse {
        0%, 100% { opacity: 1; }
        50% { opacity: 0.6; }
    }

    .sidebar-status-box {
        padding: 16px;
        background: rgba(0, 0, 0, 0.25);
        border-top: 1px solid var(--border-color, #1f293d);
    }

    .device-badge {
        display: flex;
        align-items: center;
        gap: 10px;
        background: #0b0f19;
        padding: 10px 12px;
        border-radius: 6px;
        border: 1px solid #1e293b;
    }

    .status-indicator {
        width: 10px;
        height: 10px;
        border-radius: 50%;
        background: #ef4444;
        box-shadow: 0 0 8px rgba(239, 68, 68, 0.6);
    }

    .status-indicator.online {
        background: #10b981;
        box-shadow: 0 0 8px rgba(16, 185, 129, 0.8);
    }

    .device-meta {
        display: flex;
        flex-direction: column;
    }

    .device-label {
        font-size: 0.65rem;
        color: #64748b;
        text-transform: uppercase;
        font-weight: 700;
    }

    .device-id-val {
        font-size: 0.8rem;
        color: #cbd5e1;
        font-family: monospace;
    }

    .platform-meta {
        display: flex;
        justify-content: space-between;
        margin-top: 10px;
        font-size: 0.7rem;
        color: #475569;
        font-weight: 600;
    }
</style>
