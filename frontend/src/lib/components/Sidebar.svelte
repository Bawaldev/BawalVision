<script lang="ts">
    interface Props {
        activeTab: string;
        onSelectTab: (tab: string) => void;
        activeDevice?: string;
        isOnline?: boolean;
    }

    let { activeTab = 'Dashboard', onSelectTab, activeDevice = 'ESP32S3_441BF6C95678', isOnline = true }: Props = $props();

    const menuItems = [
        { id: 'Dashboard', label: 'Dashboard', icon: 'M3 12l2-2m0 0l7-7 7 7M5 10v10a1 1 0 001 1h3m10-11l2 2m-2-2v10a1 1 0 01-1 1h-3m-6 0a1 1 0 001-1v-4a1 1 0 011-1h2a1 1 0 011 1v4a1 1 0 001 1m-6 0h6' },
        { id: 'Telemetry Analytics & Result Log', label: 'Telemetry Analytics & Result Log', icon: 'M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z' },
        { id: 'System & Cloud Settings', label: 'System & Cloud Settings', icon: 'M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z M15 12a3 3 0 11-6 0 3 3 0 016 0z' }
    ];
</script>

<aside class="sidebar-container">
    <!-- Brand Header (Matching Wireframe: Sistem MBG Mateng) -->
    <div class="brand-box">
        <div class="brand-header-inner">
            <div class="brand-emblem">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="emblem-icon">
                    <circle cx="12" cy="12" r="10" stroke-width="1.8"/>
                    <line x1="4.93" y1="4.93" x2="19.07" y2="19.07" stroke-width="1.8"/>
                    <line x1="19.07" y1="4.93" x2="4.93" y2="19.07" stroke-width="1.8"/>
                </svg>
            </div>
            <div class="brand-text">
                <h2>Sistem<br /><span>MBG Mateng</span></h2>
                <div class="device-pill">
                    <span class="pulse-dot" class:online={isOnline}></span>
                    <span class="station-code">BawalVision QC</span>
                </div>
            </div>
        </div>
    </div>

    <!-- Navigation Menu (Lovelace Rounded Chips) -->
    <nav class="nav-menu">
        <div class="nav-group-label">OVERVIEW</div>
        {#each menuItems as item}
            <button
                type="button"
                class="nav-btn"
                class:active={activeTab === item.id}
                onclick={() => onSelectTab(item.id)}
            >
                <div class="icon-circle">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="nav-svg">
                        <path stroke-linecap="round" stroke-linejoin="round" d={item.icon} />
                    </svg>
                </div>
                <span class="nav-text">{item.label}</span>
                {#if activeTab === item.id}
                    <div class="active-pill-marker"></div>
                {/if}
            </button>
        {/each}
    </nav>

    <!-- Sidebar Bottom IoT Device Card -->
    <div class="sidebar-status-box">
        <div class="device-card">
            <div class="device-card-header">
                <div class="chip-icon">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="microchip-svg">
                        <path stroke-linecap="round" stroke-linejoin="round" d="M9 3v2m6-2v2M9 19v2m6-2v2M5 9H3m2 6H3m18-6h-2m2 6h-2M7 19h10a2 2 0 002-2V7a2 2 0 00-2-2H7a2 2 0 00-2 2v10a2 2 0 002 2zM9 9h6v6H9V9z" />
                    </svg>
                </div>
                <div class="device-meta">
                    <span class="device-title">ESP32-S3 IoT Node</span>
                    <strong class="device-id-val">{activeDevice.slice(0, 16)}</strong>
                </div>
            </div>
            <div class="platform-meta">
                <span>Modbus / REST</span>
                <span class="spec-tag">{isOnline ? 'Connected' : 'Offline'}</span>
            </div>
        </div>
    </div>
</aside>

<style>
    .sidebar-container {
        width: 250px;
        background: var(--bg-sidebar);
        border-right: 1px solid var(--border-card);
        display: flex;
        flex-direction: column;
        flex-shrink: 0;
        user-select: none;
        z-index: 10;
        position: relative;
    }

    .brand-box {
        padding: 20px 18px 16px;
        border-bottom: 1px solid var(--border-subtle);
    }

    .brand-header-inner {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .brand-emblem {
        width: 40px;
        height: 40px;
        background: var(--accent-blue-dim);
        border: 1px solid rgba(56, 189, 248, 0.3);
        border-radius: var(--radius-md);
        display: flex;
        align-items: center;
        justify-content: center;
        color: var(--accent-blue);
    }

    .emblem-icon {
        width: 22px;
        height: 22px;
    }

    .brand-text h2 {
        font-family: var(--font-sans);
        font-size: 1.15rem;
        font-weight: 800;
        margin: 0;
        color: var(--text-primary);
        line-height: 1.1;
    }

    .brand-text span {
        color: var(--accent-blue);
    }

    .device-pill {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        margin-top: 4px;
        background: var(--bg-card);
        padding: 2px 8px;
        border-radius: var(--radius-pill);
        border: 1px solid var(--border-subtle);
    }

    .pulse-dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
        background: var(--accent-red);
    }

    .pulse-dot.online {
        background: var(--accent-green);
    }

    .station-code {
        font-size: 0.65rem;
        font-weight: 600;
        color: var(--text-secondary);
    }

    .nav-menu {
        display: flex;
        flex-direction: column;
        gap: 6px;
        padding: 16px 14px;
        flex: 1;
    }

    .nav-group-label {
        font-size: 0.65rem;
        font-weight: 700;
        color: var(--text-muted);
        letter-spacing: 0.06em;
        padding: 4px 8px 6px;
    }

    .nav-btn {
        display: flex;
        align-items: center;
        gap: 12px;
        width: 100%;
        padding: 10px 12px;
        background: transparent;
        border: 1px solid transparent;
        border-radius: var(--radius-md);
        color: var(--text-secondary);
        font-size: 0.85rem;
        font-weight: 600;
        text-align: left;
        cursor: pointer;
        transition: all 0.15s ease;
        position: relative;
    }

    .nav-btn:hover {
        background: var(--bg-card);
        color: var(--text-primary);
    }

    .nav-btn.active {
        background: var(--bg-card);
        color: var(--text-primary);
        border-color: var(--border-card);
        box-shadow: var(--shadow-sm);
    }

    .icon-circle {
        width: 32px;
        height: 32px;
        border-radius: 50%;
        background: var(--bg-chip);
        display: flex;
        align-items: center;
        justify-content: center;
        color: var(--text-secondary);
        transition: all 0.15s ease;
    }

    .nav-btn.active .icon-circle {
        background: var(--accent-blue-dim);
        color: var(--accent-blue);
    }

    .nav-svg {
        width: 16px;
        height: 16px;
    }

    .nav-text {
        flex: 1;
        font-size: 0.82rem;
    }

    .active-pill-marker {
        width: 4px;
        height: 16px;
        background: var(--accent-blue);
        border-radius: var(--radius-pill);
    }

    .sidebar-status-box {
        padding: 14px;
        border-top: 1px solid var(--border-subtle);
    }

    .device-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-md);
        padding: 12px;
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .device-card-header {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .chip-icon {
        width: 28px;
        height: 28px;
        border-radius: var(--radius-sm);
        background: var(--bg-chip);
        display: flex;
        align-items: center;
        justify-content: center;
        color: var(--accent-blue);
    }

    .microchip-svg {
        width: 16px;
        height: 16px;
    }

    .device-meta {
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }

    .device-title {
        font-size: 0.65rem;
        color: var(--text-muted);
        font-weight: 600;
    }

    .device-id-val {
        font-size: 0.75rem;
        font-family: var(--font-mono);
        color: var(--text-primary);
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }

    .platform-meta {
        display: flex;
        justify-content: space-between;
        align-items: center;
        font-size: 0.65rem;
        color: var(--text-muted);
        font-weight: 500;
        padding-top: 4px;
        border-top: 1px solid var(--border-subtle);
    }

    .spec-tag {
        color: var(--accent-green);
        font-weight: 600;
    }
</style>
