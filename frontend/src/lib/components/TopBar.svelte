<script lang="ts">
    interface Props {
        currentTitle?: string;
        gravitonEnabled?: boolean;
        gravitonAvailable?: boolean;
        localEngineEnabled?: boolean;
        localEngineAvailable?: boolean;
        activeDevice?: string;
        fps?: number;
        latencyMs?: number;
        onToggleGraviton?: () => void;
        onToggleLocal?: () => void;
    }

    let {
        currentTitle = 'Dashboard',
        gravitonEnabled = false,
        gravitonAvailable = false,
        localEngineEnabled = true,
        localEngineAvailable = true,
        activeDevice = 'ESP32S3_441BF6C95678',
        fps = 30.0,
        latencyMs = 5.5,
        onToggleGraviton,
        onToggleLocal
    }: Props = $props();

    let currentTime = $state(new Date().toLocaleTimeString());

    setInterval(() => {
        currentTime = new Date().toLocaleTimeString();
    }, 1000);
</script>

<header class="topbar-container">
    <!-- View Title & Breadcrumb -->
    <div class="title-group">
        <div class="title-chip">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="title-icon">
                <path stroke-linecap="round" stroke-linejoin="round" d="M4 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2V6zM14 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2V6zM4 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2v-2zM14 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2v-2z" />
            </svg>
            <h1>{currentTitle}</h1>
        </div>
        <div class="status-chip">
            <span class="pulse-dot"></span>
            <span>Live Ingress</span>
        </div>
    </div>

    <!-- Compute Engine Toggle Chips (Lovelace Entity Buttons) -->
    <div class="engine-selectors">
        <button
            type="button"
            class="entity-chip"
            class:active={gravitonAvailable && gravitonEnabled}
            class:disabled={!gravitonAvailable}
            disabled={!gravitonAvailable}
            onclick={() => {
                if (gravitonAvailable && onToggleGraviton) onToggleGraviton();
            }}
        >
            <div class="chip-circle">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="chip-svg">
                    <path stroke-linecap="round" stroke-linejoin="round" d="M3 15a4 4 0 004 4h9a5 5 0 10-.1-9.999 5.002 5.002 0 00-9.78 2.096A4.001 4.001 0 003 15z" />
                </svg>
            </div>
            <div class="chip-text">
                <strong>Cloud Graviton3</strong>
                <small>{gravitonAvailable ? (gravitonEnabled ? 'Active' : 'Standby') : 'Unavailable'}</small>
            </div>
        </button>

        <button
            type="button"
            class="entity-chip"
            class:active={localEngineAvailable && localEngineEnabled}
            class:disabled={!localEngineAvailable}
            disabled={!localEngineAvailable}
            onclick={() => {
                if (localEngineAvailable && onToggleLocal) onToggleLocal();
            }}
        >
            <div class="chip-circle">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="chip-svg">
                    <path stroke-linecap="round" stroke-linejoin="round" d="M13 10V3L4 14h7v7l9-11h-7z" />
                </svg>
            </div>
            <div class="chip-text">
                <strong>Local C++ SIMD</strong>
                <small>{localEngineEnabled ? 'Active (OpenMP)' : 'Standby'}</small>
            </div>
        </button>
    </div>

    <!-- Horizontal Telemetry Quick Glance (Home Assistant Style Metric Strip) -->
    <div class="horizontal-metrics-strip">
        <div class="metric-item">
            <span class="metric-icon">📹</span>
            <span class="metric-val">{fps.toFixed(1)} <small>FPS</small></span>
        </div>
        <div class="metric-item">
            <span class="metric-icon">⚡</span>
            <span class="metric-val">{latencyMs.toFixed(1)} <small>ms</small></span>
        </div>
        <div class="metric-item clock-item">
            <span class="metric-icon">⏱</span>
            <span class="metric-val">{currentTime}</span>
        </div>
    </div>
</header>

<style>
    .topbar-container {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 14px 22px;
        background: var(--bg-sidebar);
        border-bottom: 1px solid var(--border-card);
        gap: 16px;
        flex-wrap: wrap;
        z-index: 5;
    }

    .title-group {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .title-chip {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .title-icon {
        width: 20px;
        height: 20px;
        color: var(--accent-blue);
    }

    .title-chip h1 {
        font-size: 1.15rem;
        font-weight: 700;
        margin: 0;
        color: var(--text-primary);
    }

    .status-chip {
        display: flex;
        align-items: center;
        gap: 6px;
        background: var(--bg-card);
        border: 1px solid var(--border-subtle);
        padding: 4px 10px;
        border-radius: var(--radius-pill);
        font-size: 0.7rem;
        font-weight: 600;
        color: var(--accent-green);
    }

    .pulse-dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
        background: var(--accent-green);
        box-shadow: 0 0 6px var(--accent-green);
    }

    .engine-selectors {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .entity-chip {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 6px 12px;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-md);
        cursor: pointer;
        text-align: left;
        color: var(--text-secondary);
        transition: all 0.15s ease;
    }

    .entity-chip:hover:not(.disabled) {
        background: var(--bg-card-hover);
        border-color: var(--border-highlight);
        color: var(--text-primary);
    }

    .entity-chip.active {
        background: var(--bg-card);
        border-color: var(--accent-blue);
        color: var(--text-primary);
        box-shadow: 0 0 10px rgba(56, 189, 248, 0.15);
    }

    .entity-chip.disabled {
        opacity: 0.45;
        cursor: not-allowed;
        pointer-events: none;
    }

    .chip-circle {
        width: 28px;
        height: 28px;
        border-radius: 50%;
        background: var(--bg-chip);
        display: flex;
        align-items: center;
        justify-content: center;
        color: var(--text-secondary);
        transition: all 0.15s ease;
    }

    .entity-chip.active .chip-circle {
        background: var(--accent-blue);
        color: #0f172a;
    }

    .chip-svg {
        width: 15px;
        height: 15px;
    }

    .chip-text {
        display: flex;
        flex-direction: column;
    }

    .chip-text strong {
        font-size: 0.78rem;
        font-weight: 700;
        line-height: 1.1;
    }

    .chip-text small {
        font-size: 0.65rem;
        color: var(--text-muted);
    }

    .horizontal-metrics-strip {
        display: flex;
        align-items: center;
        gap: 12px;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        padding: 6px 14px;
        border-radius: var(--radius-pill);
    }

    .metric-item {
        display: flex;
        align-items: center;
        gap: 6px;
        font-size: 0.78rem;
        font-weight: 600;
        color: var(--text-primary);
    }

    .metric-icon {
        font-size: 0.85rem;
    }

    .metric-val {
        font-family: var(--font-mono);
    }

    .metric-val small {
        font-size: 0.68rem;
        color: var(--text-muted);
        font-weight: 500;
    }

    .clock-item {
        padding-left: 6px;
        border-left: 1px solid var(--border-subtle);
        color: var(--text-secondary);
    }
</style>
