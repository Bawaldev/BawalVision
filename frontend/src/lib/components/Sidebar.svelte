<script lang="ts">
    interface Props {
        activeTab: string;
        onSelectTab: (tab: string) => void;
        activeDevice?: string;
        isOnline?: boolean;
    }

    let {
        activeTab = 'Overview',
        onSelectTab,
        activeDevice = 'ESP32S3_441BF6C95678',
        isOnline = true
    }: Props = $props();

    const menuItems = [
        {
            id: 'Overview',
            label: 'Overview',
            icon: 'M4 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2V6zM14 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2V6zM4 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2v-2zM14 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2v-2z'
        },
        {
            id: 'Dashboard',
            label: 'Live Inspection',
            icon: 'M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z'
        },
        {
            id: 'Telemetry Analytics & Result Log',
            label: 'Telemetry & Logs',
            icon: 'M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z'
        },
        {
            id: 'System & Cloud Settings',
            label: 'Settings',
            icon: 'M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z M15 12a3 3 0 11-6 0 3 3 0 016 0z'
        }
    ];
</script>

<aside class="sidebar-container">
    <!-- Brand Header (Matching Screenshot: BawalVision + Station Node 01) -->
    <div class="brand-box">
        <div class="brand-header-inner">
            <div class="brand-emblem">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="emblem-icon">
                    <path stroke-linecap="round" stroke-linejoin="round" d="M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z" />
                </svg>
            </div>
            <div class="brand-text">
                <h2>Bawal<span class="brand-accent">Vision</span></h2>
                <div class="device-pill">
                    <span class="pulse-dot" class:online={isOnline}></span>
                    <span class="station-code">Station Node 01</span>
                </div>
            </div>
        </div>
    </div>

    <!-- Navigation Menu (Exactly 3 Menus as Requested) -->
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
                <span class="spec-tag" class:online={isOnline}>{isOnline ? 'Connected' : 'Offline'}</span>
            </div>
        </div>
    </div>
</aside>

<style>
    .sidebar-container {
        width: 240px;
        background: #ffffff;
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
        width: 38px;
        height: 38px;
        background: #e8f2f9;
        border: 1px solid #cfe0ee;
        border-radius: var(--radius-md);
        display: flex;
        align-items: center;
        justify-content: center;
        color: #3679a8;
    }

    .emblem-icon {
        width: 20px;
        height: 20px;
    }

    .brand-text h2 {
        font-family: var(--font-sans);
        font-size: 1.12rem;
        font-weight: 800;
        margin: 0;
        color: var(--text-primary);
        line-height: 1.1;
    }

    .brand-accent {
        color: #3679a8;
    }

    .device-pill {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        margin-top: 4px;
        background: #f1f5f9;
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
        box-shadow: 0 0 6px var(--accent-green);
    }

    .station-code {
        font-size: 0.65rem;
        font-weight: 600;
        color: var(--text-secondary);
    }

    .nav-menu {
        display: flex;
        flex-direction: column;
        gap: 4px;
        padding: 16px 12px;
        flex: 1;
    }

    .nav-group-label {
        font-size: 0.65rem;
        font-weight: 700;
        color: var(--text-muted);
        letter-spacing: 0.06em;
        padding: 4px 10px 8px;
    }

    .nav-btn {
        display: flex;
        align-items: center;
        gap: 10px;
        width: 100%;
        padding: 9px 12px;
        background: transparent;
        border: 1px solid transparent;
        border-radius: var(--radius-md);
        color: var(--text-secondary);
        font-size: 0.82rem;
        font-weight: 600;
        text-align: left;
        cursor: pointer;
        transition: all 0.12s ease;
        position: relative;
    }

    .nav-btn:hover {
        background: #f8fafc;
        color: var(--text-primary);
    }

    .nav-btn.active {
        background: #f1f6fa;
        color: #3679a8;
        border-color: #dbe8f3;
        font-weight: 700;
    }

    .icon-circle {
        width: 30px;
        height: 30px;
        border-radius: 50%;
        background: #f1f5f9;
        display: flex;
        align-items: center;
        justify-content: center;
        color: #64748b;
        transition: all 0.12s ease;
    }

    .nav-btn.active .icon-circle {
        background: #3679a8;
        color: #ffffff;
    }

    .nav-svg {
        width: 15px;
        height: 15px;
    }

    .nav-text {
        flex: 1;
        font-size: 0.82rem;
    }

    .active-pill-marker {
        width: 3px;
        height: 16px;
        background: #3679a8;
        border-radius: var(--radius-pill);
    }

    .sidebar-status-box {
        padding: 14px;
        border-top: 1px solid var(--border-subtle);
    }

    .device-card {
        background: #f8fafc;
        border: 1px solid var(--border-card);
        border-radius: var(--radius-md);
        padding: 10px 12px;
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .device-card-header {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .chip-icon {
        width: 26px;
        height: 26px;
        border-radius: var(--radius-sm);
        background: #e8f2f9;
        display: flex;
        align-items: center;
        justify-content: center;
        color: #3679a8;
    }

    .microchip-svg {
        width: 14px;
        height: 14px;
    }

    .device-meta {
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }

    .device-title {
        font-size: 0.62rem;
        color: var(--text-muted);
        font-weight: 600;
    }

    .device-id-val {
        font-size: 0.72rem;
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

    .spec-tag.online {
        color: var(--accent-green);
        font-weight: 700;
    }

    @media (max-width: 768px) {
        .sidebar-container {
            width: 100%;
            border-right: none;
            border-bottom: 1px solid var(--border-card);
            position: sticky;
            top: 0;
            z-index: 50;
            box-shadow: 0 1px 4px rgba(15, 23, 42, 0.06);
        }

        .brand-box {
            padding: 10px 14px;
        }

        .brand-emblem {
            width: 32px;
            height: 32px;
        }

        .brand-text {
            display: flex;
            align-items: center;
            justify-content: space-between;
            flex: 1;
            gap: 8px;
        }

        .brand-text h2 {
            font-size: 1rem;
        }

        .device-pill {
            margin-top: 0;
        }

        .nav-group-label,
        .sidebar-status-box,
        .active-pill-marker {
            display: none;
        }

        .nav-menu {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 6px;
            padding: 8px 10px;
        }

        .nav-btn {
            justify-content: center;
            padding: 7px 8px;
            gap: 6px;
            text-align: center;
        }

        .icon-circle {
            width: 24px;
            height: 24px;
            flex-shrink: 0;
        }

        .nav-svg {
            width: 13px;
            height: 13px;
        }

        .nav-text {
            flex: initial;
            font-size: 0.76rem;
            white-space: nowrap;
        }
    }

    @media (max-width: 400px) {
        .nav-btn {
            flex-direction: column;
            gap: 3px;
            padding: 6px 4px;
        }

        .nav-text {
            font-size: 0.68rem;
        }
    }
</style>
