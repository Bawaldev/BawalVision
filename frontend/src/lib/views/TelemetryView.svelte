<script lang="ts">
    import type { TelemetryPayload } from '../types';

    interface Props {
        currentTelemetry?: TelemetryPayload;
        history?: TelemetryPayload[];
    }

    let { currentTelemetry, history = [] }: Props = $props();

    let chartPoints = $derived(
        history.length > 1
            ? history.map((pt, idx) => {
                const x = (idx / (history.length - 1)) * 580 + 10;
                // Normalize VOC resistance from 0 to 150 kOhm
                const norm = Math.max(0, Math.min(1, pt.gas_voc / 150));
                const y = 140 - (norm * 110);
                return `${x.toFixed(1)},${y.toFixed(1)}`;
            }).join(' ')
            : ''
    );
</script>

<div class="telemetry-view-container">
    <div class="view-header">
        <div>
            <h2>Telemetry Analytics</h2>
            <p class="muted">Live IoT Sensor Readings & Environmental Stability Profile (BME680 Gas/Humidity/Temp)</p>
        </div>
        <div class="header-badges">
            <span class="badge blue">NODE: {currentTelemetry?.device_id || 'ESP32-S3'}</span>
            <span class="badge green">SAMPLING: 100 Hz</span>
        </div>
    </div>

    <!-- 4 Main Telemetry Cards -->
    <div class="telemetry-metric-cards">
        <div class="card">
            <span class="label">TEMPERATURE</span>
            <div class="val-group">
                <strong class="val orange">{currentTelemetry?.temperature != null && currentTelemetry.temperature > 0 ? currentTelemetry.temperature.toFixed(1) : '—'}</strong>
                <span class="unit">°C</span>
            </div>
            <small class="hint">Optimal Food Serving Temp: 20°C - 30°C</small>
        </div>

        <div class="card">
            <span class="label">RELATIVE HUMIDITY</span>
            <div class="val-group">
                <strong class="val blue">{currentTelemetry?.humidity != null && currentTelemetry.humidity > 0 ? currentTelemetry.humidity.toFixed(1) : '—'}</strong>
                <span class="unit">%Rh</span>
            </div>
            <small class="hint">Target Food Condensation: &lt; 75%</small>
        </div>

        <div class="card">
            <span class="label">GAS VOC RESISTANCE</span>
            <div class="val-group">
                <strong class="val cyan">{currentTelemetry?.gas_voc != null && currentTelemetry.gas_voc > 0 ? currentTelemetry.gas_voc.toFixed(1) : '—'}</strong>
                <span class="unit">kΩ</span>
            </div>
            <small class="hint">Freshness Baseline: &gt; 80 kΩ</small>
        </div>

        <div class="card">
            <span class="label">BAROMETRIC PRESSURE</span>
            <div class="val-group">
                <strong class="val purple">{currentTelemetry?.pressure != null && currentTelemetry.pressure > 0 ? currentTelemetry.pressure.toFixed(1) : '—'}</strong>
                <span class="unit">hPa</span>
            </div>
            <small class="hint">Atmospheric Normal (Sea Level)</small>
        </div>
    </div>

    <!-- Time Series Analytics SVG Chart -->
    <div class="chart-panel">
        <div class="chart-header">
            <h3>VOC Gas Resistance & Spoilage Trend ({history.length} Packets Recorded)</h3>
            <span class="legend">
                <span class="legend-dot cyan"></span> Live Gas VOC (kΩ)
            </span>
        </div>
        <div class="svg-chart-box">
            {#if history.length > 1}
                <svg viewBox="0 0 600 160" class="trend-svg" preserveAspectRatio="none">
                    <defs>
                        <linearGradient id="vocGrad" x1="0" y1="0" x2="0" y2="1">
                            <stop offset="0%" stop-color="#00f0ff" stop-opacity="0.3" />
                            <stop offset="100%" stop-color="#00f0ff" stop-opacity="0.0" />
                        </linearGradient>
                    </defs>

                    <!-- Grid lines -->
                    {#each [30, 70, 110, 150] as y}
                        <line x1="0" y1={y} x2="600" y2={y} stroke="#1e293b" stroke-width="1" />
                    {/each}

                    <!-- Area Fill & Polyline -->
                    <polygon
                        points={`10,140 ${chartPoints} 590,140`}
                        fill="url(#vocGrad)"
                    />
                    <polyline
                        points={chartPoints}
                        fill="none"
                        stroke="#00f0ff"
                        stroke-width="2.5"
                        stroke-linecap="round"
                        stroke-linejoin="round"
                    />

                    <!-- Point Circles -->
                    {#each history as pt, idx}
                        {@const cx = (idx / (history.length - 1)) * 580 + 10}
                        {@const norm = Math.max(0, Math.min(1, pt.gas_voc / 150))}
                        {@const cy = 140 - (norm * 110)}
                        <circle cx={cx} cy={cy} r="4" fill="#00f0ff" stroke="#0b0f19" stroke-width="2" />
                    {/each}
                </svg>
            {:else}
                <div class="empty-chart">
                    <span>Awaiting continuous telemetry packets from ESP32-S3 (POST /api/v1/telemetry)...</span>
                </div>
            {/if}
        </div>
    </div>
</div>

<style>
    .telemetry-view-container {
        display: flex;
        flex-direction: column;
        gap: 18px;
    }

    .view-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 12px;
        border-bottom: 1px solid var(--border-color, #1f293d);
    }

    .view-header h2 {
        margin: 0;
        font-size: 1.3rem;
        color: #ffffff;
    }

    .muted {
        margin: 4px 0 0;
        font-size: 0.8rem;
        color: #64748b;
    }

    .header-badges {
        display: flex;
        gap: 8px;
    }

    .badge {
        font-size: 0.7rem;
        font-family: monospace;
        font-weight: 700;
        padding: 4px 10px;
        border-radius: 4px;
    }

    .badge.blue {
        background: rgba(59, 130, 246, 0.15);
        color: #60a5fa;
        border: 1px solid rgba(59, 130, 246, 0.3);
    }

    .badge.green {
        background: rgba(16, 185, 129, 0.15);
        color: #10b981;
        border: 1px solid rgba(16, 185, 129, 0.3);
    }

    .telemetry-metric-cards {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
        gap: 14px;
    }

    .card {
        background: var(--bg-panel, #121824);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
        padding: 16px;
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .card .label {
        font-size: 0.65rem;
        font-weight: 700;
        color: #64748b;
        letter-spacing: 0.05em;
    }

    .val-group {
        display: flex;
        align-items: baseline;
        gap: 6px;
    }

    .val {
        font-size: 1.6rem;
        font-family: monospace;
        font-weight: 800;
    }

    .val.orange { color: #f59e0b; }
    .val.blue { color: #3b82f6; }
    .val.cyan { color: #00f0ff; }
    .val.purple { color: #a855f7; }

    .unit {
        font-size: 0.9rem;
        color: #94a3b8;
        font-weight: 600;
    }

    .hint {
        font-size: 0.65rem;
        color: #475569;
    }

    .chart-panel {
        background: var(--bg-panel, #121824);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
        padding: 18px;
        display: flex;
        flex-direction: column;
        gap: 12px;
    }

    .chart-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .chart-header h3 {
        margin: 0;
        font-size: 0.95rem;
        color: #f1f5f9;
    }

    .legend {
        display: flex;
        align-items: center;
        gap: 6px;
        font-size: 0.75rem;
        color: #94a3b8;
    }

    .legend-dot.cyan {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #00f0ff;
    }

    .svg-chart-box {
        width: 100%;
        height: 180px;
        background: #080c14;
        border: 1px solid #1e293b;
        border-radius: 6px;
        padding: 10px;
        display: flex;
        align-items: center;
        justify-content: center;
    }

    .empty-chart {
        color: #64748b;
        font-family: monospace;
        font-size: 0.8rem;
        text-align: center;
    }

    .trend-svg {
        width: 100%;
        height: 100%;
    }
</style>
