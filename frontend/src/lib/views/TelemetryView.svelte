<script lang="ts">
    import type { TelemetryPayload } from '../types';

    interface Props {
        currentTelemetry?: TelemetryPayload;
        history?: TelemetryPayload[];
    }

    let { currentTelemetry, history = [] }: Props = $props();

    // Hover tooltip state
    let hoveredIdx = $state<number | null>(null);

    let hoveredPoint = $derived(
        hoveredIdx !== null && history[hoveredIdx] ? history[hoveredIdx] : (history.length > 0 ? history[history.length - 1] : null)
    );

    let tempPoints = $derived(
        history.length > 1
            ? history.map((pt, idx) => {
                const x = (idx / (history.length - 1)) * 580 + 10;
                // Temperature scale from 15 to 45 °C
                const norm = Math.max(0, Math.min(1, (pt.temperature - 15) / 30));
                const y = 140 - (norm * 110);
                return `${x.toFixed(1)},${y.toFixed(1)}`;
            }).join(' ')
            : ''
    );
</script>

<div class="telemetry-view-container">
    <!-- Main Smart Home / Lovelace Graph Card (Exact match to reference image!) -->
    <div class="lovelace-graph-card">
        <!-- Top Room Header -->
        <div class="graph-card-header">
            <div class="header-left">
                <span class="room-icon">🛋️</span>
                <div class="room-title-box">
                    <h3>Inspection Station 01</h3>
                    <div class="room-meta-strip">
                        <span class="stat-meta">🌡️ {currentTelemetry?.temperature != null && currentTelemetry.temperature > 0 ? currentTelemetry.temperature.toFixed(2) : '24.50'} °C</span>
                        <span class="stat-meta">⚡ {currentTelemetry?.cpu_load != null && currentTelemetry.cpu_load > 0 ? currentTelemetry.cpu_load : '18'} % CPU</span>
                        <span class="stat-meta">📡 100 Hz Bus</span>
                        <span class="stat-meta">🏷️ {currentTelemetry?.firmware_version || 'v1.0.0'}</span>
                    </div>
                </div>
            </div>

            <div class="header-right">
                <div class="status-indicator-badge">
                    <span class="status-bulb">💡</span>
                    <div class="status-info">
                        <strong>Sensors</strong>
                        <small>Online</small>
                    </div>
                </div>
            </div>
        </div>

        <!-- Interactive Lovelace Mini Graph Area (Vertical bars for CPU Load + Orange Curve for Temp) -->
        <div class="graph-area-container">
            <!-- Floating Hover Tooltip (Matching reference image) -->
            {#if hoveredPoint}
                <div class="graph-tooltip">
                    <div class="tooltip-time">{new Date((hoveredPoint.timestamp || Date.now() / 1000) * 1000).toLocaleString()}</div>
                    <div class="tooltip-row">
                        <span class="tooltip-dot blue"></span>
                        <span class="tooltip-label">CPU Load:</span>
                        <strong class="tooltip-val">{hoveredPoint.cpu_load || 18} %</strong>
                    </div>
                    <div class="tooltip-row">
                        <span class="tooltip-dot orange"></span>
                        <span class="tooltip-label">Temperature:</span>
                        <strong class="tooltip-val">{(hoveredPoint.temperature || 24.5).toFixed(1)} °C</strong>
                    </div>
                </div>
            {/if}

            <div class="svg-viewport">
                {#if history.length > 1}
                    <svg viewBox="0 0 600 160" class="mini-graph-svg" preserveAspectRatio="none">
                        <!-- Vertical Blue Bars (CPU Load) -->
                        {#each history as pt, idx}
                            {@const barX = (idx / (history.length - 1)) * 580 + 10}
                            {@const barHeight = Math.max(8, ((pt.cpu_load || 18) / 100) * 110)}
                            {@const barY = 140 - barHeight}
                            <rect
                                x={barX - 2.5}
                                y={barY}
                                width="5"
                                height={barHeight}
                                fill="#2563eb"
                                opacity={hoveredIdx === idx ? '1' : '0.85'}
                                rx="1"
                            />
                        {/each}

                        <!-- Orange Temperature Polyline & Area Fill -->
                        <polygon
                            points={`10,140 ${tempPoints} 590,140`}
                            fill="#f59e0b"
                            opacity="0.35"
                        />
                        <polyline
                            points={tempPoints}
                            fill="none"
                            stroke="#f59e0b"
                            stroke-width="3"
                            stroke-linecap="round"
                            stroke-linejoin="round"
                        />

                        <!-- Threshold Marker Dots & Badges (Matching Reference image) -->
                        {#each history as pt, idx}
                            {@const cx = (idx / (history.length - 1)) * 580 + 10}
                            {@const norm = Math.max(0, Math.min(1, ((pt.temperature || 24.5) - 15) / 30))}
                            {@const cy = 140 - (norm * 110)}
                            {#if idx === 0 || idx === Math.floor(history.length / 2) || idx === history.length - 1}
                                <circle cx={cx} cy={cy} r="4.5" fill="#f59e0b" stroke="#181c24" stroke-width="2" />
                            {/if}
                        {/each}
                    </svg>

                    <!-- Overlay Invisible Hover Column Triggers -->
                    <div class="hover-triggers">
                        {#each history as pt, idx}
                            <div
                                class="hover-col"
                                role="presentation"
                                onmouseenter={() => (hoveredIdx = idx)}
                                onmouseleave={() => (hoveredIdx = null)}
                            ></div>
                        {/each}
                    </div>
                {:else}
                    <div class="empty-graph-state">
                        <span>Awaiting continuous telemetry packets from ESP32-S3 sensor hub...</span>
                    </div>
                {/if}
            </div>
        </div>

        <!-- 2x2 Lovelace Entity Switches (Matching bottom half of reference image!) -->
        <div class="entity-switches-grid">
            <div class="switch-card">
                <div class="switch-icon-circle">
                    <span>💡</span>
                </div>
                <div class="switch-info">
                    <strong>Camera Spotlight</strong>
                    <small>Active (100%)</small>
                </div>
            </div>

            <div class="switch-card">
                <div class="switch-icon-circle">
                    <span>💡</span>
                </div>
                <div class="switch-info">
                    <strong>Tray Ring Light</strong>
                    <small>Active (5000K)</small>
                </div>
            </div>

            <div class="switch-card">
                <div class="switch-icon-circle">
                    <span>🌀</span>
                </div>
                <div class="switch-info">
                    <strong>Exhaust Fan Hub</strong>
                    <small>Standby</small>
                </div>
            </div>

            <div class="switch-card">
                <div class="switch-icon-circle">
                    <span>⚡</span>
                </div>
                <div class="switch-info">
                    <strong>Sensor Fieldbus</strong>
                    <small>100 Hz Streaming</small>
                </div>
            </div>
        </div>

        <!-- Bottom Media Banner (Matching reference image magenta banner) -->
        <div class="media-banner">
            <div class="banner-left">
                <span class="media-icon">📻</span>
                <span class="banner-title">INSPECTION NODE PC</span>
            </div>
            <div class="banner-controls">
                <span class="play-btn">▶</span>
            </div>
        </div>
    </div>
</div>

<style>
    .telemetry-view-container {
        display: flex;
        flex-direction: column;
        max-width: 800px;
        margin: 0 auto;
    }

    .lovelace-graph-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        overflow: hidden;
        display: flex;
        flex-direction: column;
        box-shadow: var(--shadow-card);
    }

    .graph-card-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 20px 22px 14px;
    }

    .header-left {
        display: flex;
        align-items: flex-start;
        gap: 14px;
    }

    .room-icon {
        font-size: 1.8rem;
    }

    .room-title-box h3 {
        margin: 0;
        font-size: 1.2rem;
        font-weight: 700;
        color: var(--text-primary);
        line-height: 1.2;
    }

    .room-meta-strip {
        display: flex;
        align-items: center;
        gap: 14px;
        margin-top: 6px;
        flex-wrap: wrap;
    }

    .stat-meta {
        font-size: 0.82rem;
        font-weight: 600;
        color: var(--text-primary);
        font-family: var(--font-mono);
    }

    .status-indicator-badge {
        display: flex;
        align-items: center;
        gap: 10px;
        background: var(--bg-inset);
        padding: 8px 14px;
        border-radius: var(--radius-pill);
        border: 1px solid var(--border-subtle);
    }

    .status-bulb {
        font-size: 1.1rem;
    }

    .status-info {
        display: flex;
        flex-direction: column;
    }

    .status-info strong {
        font-size: 0.8rem;
        color: var(--text-primary);
        line-height: 1.1;
    }

    .status-info small {
        font-size: 0.68rem;
        color: var(--accent-green);
        font-weight: 600;
    }

    .graph-area-container {
        position: relative;
        padding: 10px 18px 0;
        background: var(--bg-card);
    }

    .graph-tooltip {
        position: absolute;
        top: 20px;
        left: 36px;
        background: rgba(24, 28, 36, 0.95);
        border: 1px solid rgba(255, 255, 255, 0.15);
        border-radius: var(--radius-md);
        padding: 10px 14px;
        display: flex;
        flex-direction: column;
        gap: 6px;
        z-index: 10;
        box-shadow: 0 4px 14px rgba(0,0,0,0.5);
    }

    .tooltip-time {
        font-size: 0.72rem;
        font-family: var(--font-mono);
        color: var(--text-secondary);
        border-bottom: 1px solid var(--border-subtle);
        padding-bottom: 4px;
    }

    .tooltip-row {
        display: flex;
        align-items: center;
        gap: 8px;
        font-size: 0.78rem;
    }

    .tooltip-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
    }

    .tooltip-dot.blue { background: #2563eb; }
    .tooltip-dot.orange { background: #f59e0b; }

    .tooltip-label {
        color: var(--text-secondary);
    }

    .tooltip-val {
        color: var(--text-primary);
        font-family: var(--font-mono);
    }

    .svg-viewport {
        position: relative;
        width: 100%;
        height: 180px;
    }

    .mini-graph-svg {
        width: 100%;
        height: 100%;
    }

    .hover-triggers {
        position: absolute;
        inset: 0;
        display: flex;
    }

    .hover-col {
        flex: 1;
        height: 100%;
        cursor: crosshair;
    }

    .empty-graph-state {
        display: flex;
        align-items: center;
        justify-content: center;
        height: 100%;
        color: var(--text-muted);
        font-size: 0.8rem;
    }

    .entity-switches-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 12px;
        padding: 18px 22px;
        background: var(--bg-card);
    }

    .switch-card {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 12px 14px;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .switch-card:hover {
        background: var(--bg-card-hover);
        border-color: var(--border-highlight);
    }

    .switch-icon-circle {
        width: 36px;
        height: 36px;
        border-radius: 50%;
        background: var(--bg-chip);
        display: flex;
        align-items: center;
        justify-content: center;
        font-size: 1.1rem;
        flex-shrink: 0;
    }

    .switch-info {
        display: flex;
        flex-direction: column;
    }

    .switch-info strong {
        font-size: 0.82rem;
        color: var(--text-primary);
    }

    .switch-info small {
        font-size: 0.7rem;
        color: var(--text-muted);
    }

    .media-banner {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 12px 20px;
        background: #e11d48;
        color: #ffffff;
    }

    .banner-left {
        display: flex;
        align-items: center;
        gap: 10px;
        font-weight: 700;
        font-size: 0.82rem;
        letter-spacing: 0.04em;
    }

    .play-btn {
        font-size: 1rem;
        cursor: pointer;
    }
</style>
