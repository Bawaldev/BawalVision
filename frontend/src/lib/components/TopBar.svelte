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
        isOnline?: boolean;
        isMock?: boolean;
        memoryUsage?: string | null;
        memoryMB?: number | null;
        parallelCores?: number;
        onToggleGraviton?: () => void;
        onToggleLocal?: () => void;
    }

    let {
        currentTitle = 'Dashboard',
        gravitonEnabled = false,
        gravitonAvailable = false,
        localEngineEnabled = true,
        localEngineAvailable = true,
        activeDevice = '',
        fps = 0.0,
        latencyMs = 0.0,
        isOnline = false,
        isMock = false,
        memoryUsage = null,
        memoryMB = null,
        parallelCores = 0,
        onToggleGraviton,
        onToggleLocal
    }: Props = $props();

    let memoryDisplay = $derived.by(() => {
        if (memoryUsage && memoryUsage.trim() !== '') return memoryUsage;
        if (memoryMB != null && memoryMB > 0) {
            return `Memory usage: ${memoryMB >= 1024 ? (memoryMB / 1024).toFixed(2) + ' GB' : Math.round(memoryMB) + ' MB'}`;
        }
        return isOnline ? 'Memory: In Sync' : 'Memory: Offline';
    });

    let currentTime = $state(new Date().toLocaleTimeString('en-US', { hour12: true }));

    $effect(() => {
        const interval = setInterval(() => {
            currentTime = new Date().toLocaleTimeString('en-US', { hour12: true });
        }, 1000);
        return () => clearInterval(interval);
    });
</script>

<header class="topbar-container">
    <!-- View Title & Breadcrumb -->
    <div class="title-group">
        <div class="title-chip">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="title-icon">
                <path stroke-linecap="round" stroke-linejoin="round" d="M4 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2V6zM14 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2V6zM4 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2v-2zM14 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2v-2z" />
            </svg>
            {#key currentTitle}
                <h1>{currentTitle}</h1>
            {/key}
        </div>
        <div class="status-chip" class:offline={!isOnline} class:mock={isOnline && isMock}>
            <span class="pulse-dot" class:online={isOnline && !isMock} class:mock={isOnline && isMock}></span>
            <span>{!isOnline ? 'Offline' : (isMock ? 'Mock Simulation' : 'Live Ingress')}</span>
        </div>
    </div>

    <!-- Compute Engine Toggle Chips -->
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
            title={gravitonAvailable ? 'Toggle cloud inference' : 'Cloud inference offline'}
        >
            <div class="chip-circle">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="chip-svg">
                    <path stroke-linecap="round" stroke-linejoin="round" d="M3 15a4 4 0 004 4h9a5 5 0 10-.1-9.999 5.002 5.002 0 00-9.78 2.096A4.001 4.001 0 003 15z" />
                </svg>
            </div>
            <div class="chip-text">
                <strong>Cloud Inference</strong>
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
            title="Toggle Local C++ SIMD Inference Engine"
        >
            <div class="chip-circle">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="chip-svg">
                    <path stroke-linecap="round" stroke-linejoin="round" d="M13 10V3L4 14h7v7l9-11h-7z" />
                </svg>
            </div>
            <div class="chip-text">
                <strong>Local C++ SIMD</strong>
                <small>{localEngineEnabled ? (parallelCores > 0 ? `Active (${parallelCores}C OpenMP)` : 'Active (OpenMP)') : 'Standby'}</small>
            </div>
        </button>
    </div>

    <!-- Horizontal Telemetry Quick Glance -->
    <div class="horizontal-metrics-strip">
        <div class="metric-item memory-item">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="metric-svg">
                <path stroke-linecap="round" stroke-linejoin="round" d="M9 3v2m6-2v2M9 19v2m6-2v2M5 9H3m2 6H3m18-6h-2m2 6h-2M7 19h10a2 2 0 002-2V7a2 2 0 00-2-2H7a2 2 0 00-2 2v10a2 2 0 002 2z" />
            </svg>
            <span class="metric-val">{memoryDisplay}</span>
        </div>
        <div class="metric-item fps-item">
            <span class="metric-dot green"></span>
            <span class="metric-val"><strong>{fps.toFixed(1)}</strong> <small>FPS</small></span>
        </div>
        <div class="metric-item latency-item">
            <span class="metric-dot blue"></span>
            <span class="metric-val"><strong>{latencyMs.toFixed(1)}</strong> <small>ms</small></span>
        </div>
        <div class="metric-item clock-item">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="metric-svg clock-svg">
                <circle cx="12" cy="12" r="9" />
                <path d="M12 7v5l3 3" />
            </svg>
            <span class="metric-val clock-val">{currentTime}</span>
        </div>
    </div>
</header>

<style>
    .topbar-container {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 12px 24px;
        background: #ffffff;
        border-bottom: 1px solid var(--border-card);
        gap: 16px;
        flex-wrap: wrap;
        z-index: 10;
        min-height: 60px;
    }

    .title-group {
        display: flex;
        align-items: center;
        gap: 14px;
        flex-shrink: 0;
    }

    .title-chip {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .title-icon {
        width: 20px;
        height: 20px;
        color: #3679a8;
    }

    .title-chip h1 {
        font-size: 1.15rem;
        font-weight: 700;
        margin: 0;
        color: #1e293b;
        letter-spacing: -0.01em;
        animation: titleIn 0.2s cubic-bezier(0.16, 1, 0.3, 1) both;
    }

    @keyframes titleIn {
        0% {
            opacity: 0;
            transform: translate3d(0, -4px, 0);
        }
        100% {
            opacity: 1;
            transform: translate3d(0, 0, 0);
        }
    }

    .status-chip {
        display: flex;
        align-items: center;
        gap: 6px;
        background: #f1f5f9;
        border: 1px solid #e2e8f0;
        padding: 4px 10px;
        border-radius: var(--radius-pill);
        font-size: 0.72rem;
        font-weight: 600;
        color: #16a34a;
    }

    .status-chip.offline {
        color: #64748b;
    }

    .status-chip.mock {
        color: #d97706;
        background: #fffbeb;
        border-color: #fde68a;
    }

    .pulse-dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
        background: #64748b;
    }

    .pulse-dot.online {
        background: #16a34a;
        box-shadow: 0 0 6px rgba(22, 163, 74, 0.6);
    }

    .pulse-dot.mock {
        background: #d97706;
        box-shadow: 0 0 6px rgba(217, 119, 6, 0.6);
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
        padding: 6px 14px;
        background: #f8fafc;
        border: 1px solid #d8e0ea;
        border-radius: 6px;
        cursor: pointer;
        text-align: left;
        color: #475569;
        transition: all 0.15s ease;
    }

    .entity-chip:hover:not(.disabled) {
        background: #ffffff;
        border-color: #3679a8;
        color: #1e293b;
        box-shadow: 0 1px 3px rgba(15, 23, 42, 0.08);
    }

    .entity-chip.active {
        background: #f0f7fc;
        border-color: #3679a8;
        color: #1e293b;
        box-shadow: 0 1px 3px rgba(54, 121, 168, 0.15);
    }

    .entity-chip.disabled {
        opacity: 0.5;
        cursor: not-allowed;
        background: #f1f5f9;
        border-color: #e2e8f0;
    }

    .chip-circle {
        width: 28px;
        height: 28px;
        border-radius: 50%;
        background: #e2e8f0;
        display: flex;
        align-items: center;
        justify-content: center;
        color: #64748b;
        transition: all 0.15s ease;
        flex-shrink: 0;
    }

    .entity-chip.active .chip-circle {
        background: #3679a8;
        color: #ffffff;
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
        line-height: 1.15;
        color: #1e293b;
    }

    .chip-text small {
        font-size: 0.65rem;
        color: #64748b;
        font-weight: 500;
    }

    .entity-chip.active .chip-text small {
        color: #3679a8;
        font-weight: 600;
    }

    .horizontal-metrics-strip {
        display: flex;
        align-items: center;
        gap: 14px;
        background: #f8fafc;
        border: 1px solid #d8e0ea;
        padding: 6px 14px;
        border-radius: var(--radius-pill);
        flex-shrink: 0;
    }

    .metric-item {
        display: flex;
        align-items: center;
        gap: 6px;
        font-size: 0.75rem;
        font-weight: 600;
        color: #1e293b;
    }

    .metric-svg {
        width: 14px;
        height: 14px;
        color: #64748b;
    }

    .metric-dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
    }

    .metric-dot.green {
        background: #16a34a;
    }

    .metric-dot.blue {
        background: #3679a8;
    }

    .metric-val {
        font-family: var(--font-mono);
        font-size: 0.75rem;
    }

    .metric-val strong {
        font-weight: 700;
        color: #1e293b;
    }

    .metric-val small {
        font-size: 0.65rem;
        color: #64748b;
        font-weight: 500;
    }

    .clock-item {
        padding-left: 10px;
        border-left: 1px solid #d8e0ea;
        color: #475569;
    }

    .clock-svg {
        width: 13px;
        height: 13px;
        color: #64748b;
    }

    .clock-val {
        font-size: 0.74rem;
        color: #475569;
        font-weight: 600;
    }

    @media (max-width: 1050px) {
        .engine-selectors {
            order: 3;
            width: 100%;
            justify-content: flex-start;
        }
    }

    @media (max-width: 768px) {
        .topbar-container {
            padding: 10px 12px;
            gap: 10px;
        }

        .title-group {
            width: 100%;
            justify-content: space-between;
            gap: 8px;
        }

        .title-chip {
            min-width: 0;
        }

        .title-chip h1 {
            font-size: 0.96rem;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
        }

        .engine-selectors {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 8px;
            width: 100%;
        }

        .entity-chip {
            padding: 6px 10px;
            gap: 8px;
            min-width: 0;
        }

        .chip-text {
            min-width: 0;
        }

        .chip-text strong,
        .chip-text small {
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
        }

        .horizontal-metrics-strip {
            width: 100%;
            justify-content: space-between;
            flex-wrap: wrap;
            gap: 8px;
            padding: 6px 12px;
            border-radius: var(--radius-md);
        }
    }

    @media (max-width: 480px) {
        .memory-item {
            display: none;
        }

        .engine-selectors {
            grid-template-columns: 1fr;
        }
    }
</style>
