<script lang="ts">
    interface Props {
        currentTitle?: string;
        gravitonEnabled?: boolean;
        localEngineEnabled?: boolean;
        activeDevice?: string;
        fps?: number;
        latencyMs?: number;
        onToggleGraviton?: () => void;
        onToggleLocal?: () => void;
    }

    let {
        currentTitle = 'Dashboard',
        gravitonEnabled = true,
        localEngineEnabled = true,
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
        <div class="title-box">
            <h1>{currentTitle}</h1>
            <span class="live-tag">REALTIME INGRESS</span>
        </div>
    </div>

    <!-- Engine Selectors (Wireframe: AWS Graviton3 / Local Engine) -->
    <div class="engine-selectors">
        <button
            type="button"
            class="engine-card"
            class:selected={gravitonEnabled}
            onclick={onToggleGraviton}
        >
            <div class="checkbox-indicator" class:checked={gravitonEnabled}>
                {#if gravitonEnabled}✓{/if}
            </div>
            <div class="engine-info">
                <strong>AWS Graviton3</strong>
                <small>ARM64 + SVE Vectorized</small>
            </div>
        </button>

        <button
            type="button"
            class="engine-card"
            class:selected={localEngineEnabled}
            onclick={onToggleLocal}
        >
            <div class="checkbox-indicator" class:checked={localEngineEnabled}>
                {#if localEngineEnabled}✓{/if}
            </div>
            <div class="engine-info">
                <strong>Local Engine</strong>
                <small>OpenMP SIMD C++20</small>
            </div>
        </button>
    </div>

    <!-- Top Right Telemetry & Status Badges -->
    <div class="top-stats">
        <div class="stat-pill">
            <span class="pill-label">STREAM FPS</span>
            <strong class="pill-val green">{fps.toFixed(1)}</strong>
        </div>
        <div class="stat-pill">
            <span class="pill-label">LATENCY</span>
            <strong class="pill-val cyan">{latencyMs.toFixed(1)} ms</strong>
        </div>
        <div class="stat-pill time-pill">
            <span class="pill-label">SYSTEM CLOCK</span>
            <strong class="pill-val">{currentTime}</strong>
        </div>
    </div>
</header>

<style>
    .topbar-container {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 12px 24px;
        background: var(--bg-topbar, #0e131f);
        border-bottom: 1px solid var(--border-color, #1f293d);
        gap: 20px;
        flex-wrap: wrap;
    }

    .title-group {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .title-box h1 {
        font-size: 1.4rem;
        font-weight: 800;
        letter-spacing: -0.02em;
        margin: 0;
        color: #ffffff;
        display: inline-block;
        margin-right: 10px;
    }

    .live-tag {
        display: inline-block;
        font-size: 0.65rem;
        font-weight: 700;
        color: #00f0ff;
        background: rgba(0, 240, 255, 0.1);
        border: 1px solid rgba(0, 240, 255, 0.3);
        padding: 3px 8px;
        border-radius: 4px;
        letter-spacing: 0.08em;
    }

    .engine-selectors {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .engine-card {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 8px 14px;
        background: #141b2d;
        border: 1px solid #23304a;
        border-radius: 6px;
        cursor: pointer;
        text-align: left;
        color: #94a3b8;
        transition: all 0.2s ease;
    }

    .engine-card:hover {
        border-color: #3b82f6;
        background: #19233c;
    }

    .engine-card.selected {
        border-color: #00f0ff;
        background: rgba(0, 240, 255, 0.08);
        color: #ffffff;
        box-shadow: 0 0 10px rgba(0, 240, 255, 0.1);
    }

    .checkbox-indicator {
        width: 18px;
        height: 18px;
        border-radius: 4px;
        border: 1.5px solid #475569;
        display: flex;
        align-items: center;
        justify-content: center;
        font-size: 0.75rem;
        font-weight: 900;
        color: #000;
        background: transparent;
    }

    .checkbox-indicator.checked {
        background: #00f0ff;
        border-color: #00f0ff;
    }

    .engine-info strong {
        display: block;
        font-size: 0.85rem;
        line-height: 1.1;
    }

    .engine-info small {
        font-size: 0.65rem;
        color: #64748b;
    }

    .top-stats {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .stat-pill {
        display: flex;
        flex-direction: column;
        background: #121824;
        padding: 6px 12px;
        border-radius: 6px;
        border: 1px solid #1e293b;
        min-width: 85px;
    }

    .pill-label {
        font-size: 0.6rem;
        font-weight: 700;
        color: #64748b;
        letter-spacing: 0.05em;
    }

    .pill-val {
        font-size: 0.95rem;
        font-family: monospace;
        color: #f8fafc;
    }

    .pill-val.green {
        color: #10b981;
    }

    .pill-val.cyan {
        color: #00f0ff;
    }

    .time-pill {
        min-width: 110px;
    }
</style>
