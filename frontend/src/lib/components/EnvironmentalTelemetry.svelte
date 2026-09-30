<script lang="ts">
    interface Props {
        temperature?: number;
        humidity?: number;
        pressure?: number;
        cpuLoad?: number;
    }

    let { temperature = 24.5, humidity = 65.0, pressure = 1013.25, cpuLoad = 18 }: Props = $props();
</script>

<div class="env-card">
    <div class="card-top">
        <div class="header-left">
            <span class="room-icon">🌡️</span>
            <div>
                <h4>Environmental Climate</h4>
                <p class="sub-label">BME680 Sensor Hub</p>
            </div>
        </div>
        <span class="bus-badge">I2C 0x76</span>
    </div>

    <!-- Horizontal IoT Metrics Strip (Like reference image) -->
    <div class="metrics-row">
        <div class="stat-item">
            <span class="icon">🌡️</span>
            <span class="val">{temperature.toFixed(1)} <small>°C</small></span>
        </div>
        <div class="stat-item">
            <span class="icon">💧</span>
            <span class="val">{humidity.toFixed(1)} <small>%</small></span>
        </div>
        <div class="stat-item">
            <span class="icon">⏱</span>
            <span class="val">{pressure.toFixed(1)} <small>hPa</small></span>
        </div>
        <div class="stat-item">
            <span class="icon">⚡</span>
            <span class="val">{cpuLoad} <small>%</small></span>
        </div>
    </div>

    <!-- Mini Telemetry Level Meters -->
    <div class="meters-container">
        <div class="meter-bar-item">
            <div class="meter-labels">
                <span>Temperature</span>
                <strong>{temperature.toFixed(1)} °C</strong>
            </div>
            <div class="progress-track">
                <div class="progress-fill temp" style={`width: ${Math.min(100, Math.max(0, (temperature / 50) * 100))}%`}></div>
            </div>
        </div>

        <div class="meter-bar-item">
            <div class="meter-labels">
                <span>Relative Humidity</span>
                <strong>{humidity.toFixed(1)} %RH</strong>
            </div>
            <div class="progress-track">
                <div class="progress-fill hum" style={`width: ${Math.min(100, Math.max(0, humidity))}%`}></div>
            </div>
        </div>
    </div>
</div>

<style>
    .env-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        padding: 16px;
        display: flex;
        flex-direction: column;
        gap: 14px;
        box-shadow: var(--shadow-card);
    }

    .card-top {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .header-left {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .room-icon {
        font-size: 1.3rem;
    }

    .header-left h4 {
        margin: 0;
        font-size: 0.92rem;
        font-weight: 700;
        color: var(--text-primary);
        line-height: 1.2;
    }

    .sub-label {
        margin: 0;
        font-size: 0.68rem;
        color: var(--text-muted);
    }

    .bus-badge {
        font-size: 0.68rem;
        font-weight: 600;
        background: var(--bg-chip);
        color: var(--text-secondary);
        padding: 2px 8px;
        border-radius: var(--radius-pill);
    }

    .metrics-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        background: var(--bg-inset);
        padding: 10px 14px;
        border-radius: var(--radius-md);
        border: 1px solid var(--border-subtle);
    }

    .stat-item {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .stat-item .icon {
        font-size: 0.9rem;
    }

    .stat-item .val {
        font-size: 0.85rem;
        font-weight: 700;
        color: var(--text-primary);
        font-family: var(--font-mono);
    }

    .stat-item .val small {
        font-size: 0.68rem;
        color: var(--text-muted);
        font-weight: 500;
    }

    .meters-container {
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .meter-bar-item {
        display: flex;
        flex-direction: column;
        gap: 4px;
    }

    .meter-labels {
        display: flex;
        justify-content: space-between;
        font-size: 0.72rem;
        color: var(--text-secondary);
    }

    .meter-labels strong {
        color: var(--text-primary);
        font-family: var(--font-mono);
    }

    .progress-track {
        height: 6px;
        background: var(--bg-chip);
        border-radius: var(--radius-pill);
        overflow: hidden;
    }

    .progress-fill {
        height: 100%;
        border-radius: var(--radius-pill);
        transition: width 0.4s ease;
    }

    .progress-fill.temp {
        background: var(--accent-amber);
    }

    .progress-fill.hum {
        background: var(--accent-blue);
    }
</style>
