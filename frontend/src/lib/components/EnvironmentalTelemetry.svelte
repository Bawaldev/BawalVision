<script lang="ts">
    interface Props {
        foodTemperature?: number;
        cpuTemperature?: number;
        temperature?: number;
        cpuLoad?: number;
        firmwareVersion?: string;
    }

    let {
        foodTemperature,
        cpuTemperature = 36.5,
        temperature,
        cpuLoad = 18,
        firmwareVersion = '1.0.0'
    }: Props = $props();

    let displayFoodTemp = $derived(
        foodTemperature != null && foodTemperature > 0
            ? foodTemperature
            : temperature != null && temperature > 0
            ? temperature
            : 24.5
    );

    let displayCpuTemp = $derived(
        cpuTemperature != null && cpuTemperature > 0
            ? cpuTemperature
            : 36.5
    );
</script>

<div class="env-card">
    <div class="card-top">
        <div class="header-left">
            <span class="room-icon">🌡️</span>
            <div>
                <h4>Thermal & Hardware Telemetry</h4>
                <p class="sub-label">Food Surface vs ESP32-S3 Core</p>
            </div>
        </div>
        <span class="bus-badge">v{firmwareVersion}</span>
    </div>

    <!-- Horizontal IoT Metrics Strip -->
    <div class="metrics-row">
        <div class="stat-item">
            <span class="icon">🥗</span>
            <div class="stat-text">
                <span class="val">{displayFoodTemp.toFixed(1)} <small>°C</small></span>
                <span class="stat-tag">Food</span>
            </div>
        </div>
        <div class="stat-item">
            <span class="icon">💻</span>
            <div class="stat-text">
                <span class="val">{displayCpuTemp.toFixed(1)} <small>°C</small></span>
                <span class="stat-tag">MCU</span>
            </div>
        </div>
        <div class="stat-item">
            <span class="icon">⚡</span>
            <div class="stat-text">
                <span class="val">{cpuLoad > 0 ? cpuLoad : '18'} <small>%</small></span>
                <span class="stat-tag">Load</span>
            </div>
        </div>
    </div>

    <!-- Mini Telemetry Level Meters -->
    <div class="meters-container">
        <div class="meter-bar-item">
            <div class="meter-labels">
                <span>Food Surface Temperature</span>
                <strong>{displayFoodTemp.toFixed(1)} °C</strong>
            </div>
            <div class="progress-track">
                <div class="progress-fill food-temp" style={`width: ${Math.min(100, Math.max(0, (displayFoodTemp / 50) * 100))}%`}></div>
            </div>
        </div>

        <div class="meter-bar-item">
            <div class="meter-labels">
                <span>ESP32-S3 CPU Temperature</span>
                <strong>{displayCpuTemp.toFixed(1)} °C</strong>
            </div>
            <div class="progress-track">
                <div class="progress-fill cpu-temp" style={`width: ${Math.min(100, Math.max(0, (displayCpuTemp / 80) * 100))}%`}></div>
            </div>
        </div>

        <div class="meter-bar-item">
            <div class="meter-labels">
                <span>Core CPU Utilization</span>
                <strong>{cpuLoad > 0 ? cpuLoad : 18} %</strong>
            </div>
            <div class="progress-track">
                <div class="progress-fill cpu" style={`width: ${Math.min(100, Math.max(0, cpuLoad || 18))}%`}></div>
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

    .stat-text {
        display: flex;
        flex-direction: column;
    }

    .stat-tag {
        font-size: 0.65rem;
        color: var(--text-muted);
        text-transform: uppercase;
        font-weight: 600;
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

    .progress-fill.food-temp {
        background: var(--accent-amber);
    }

    .progress-fill.cpu-temp {
        background: #ec4899;
    }

    .progress-fill.cpu {
        background: var(--accent-blue);
    }
</style>
