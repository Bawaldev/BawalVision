<script lang="ts">
    interface Props {
        isConnected?: boolean;
        cpuTemperature?: number | null;
        temperature?: number | null;
        cpuLoad?: number | null;
        firmwareVersion?: string;
    }

    let {
        isConnected = false,
        cpuTemperature = null,
        temperature = null,
        cpuLoad = null,
        firmwareVersion = '1.0.4'
    }: Props = $props();

    // ESP32-S3 real temperature, CPU temp, and CPU utility are ONLY available if the ESP device is connected
    let isDeviceActive = $derived(
        Boolean(isConnected) && temperature != null && temperature > 0
    );

    let displayRealTemp = $derived(
        isDeviceActive && temperature != null ? temperature : null
    );
</script>

<div class="env-card">
    <div class="card-top">
        <div class="header-left">
            <span class="room-icon">🌡️</span>
            <div>
                <h4>Thermal & Hardware Telemetry</h4>
                <p class="sub-label">{isDeviceActive ? 'ESP-IDF Live ESP32-S3 Real Temperature' : 'ESP-IDF Device Disconnected'}</p>
            </div>
        </div>
        <span class="bus-badge" class:offline={!isDeviceActive}>{isDeviceActive ? `v${firmwareVersion}` : 'Offline'}</span>
    </div>

    <!-- Horizontal IoT Metrics Strip -->
    <div class="metrics-row">
        <div class="stat-item" class:is-disabled={!isDeviceActive}>
            <span class="icon">🌡️</span>
            <div class="stat-text">
                {#if isDeviceActive && displayRealTemp != null}
                    <span class="val">{displayRealTemp.toFixed(1)} <small>°C</small></span>
                    <span class="stat-tag">ESP32-S3</span>
                {:else}
                    <span class="val na">-- <small>°C</small></span>
                    <span class="stat-tag offline">Offline</span>
                {/if}
            </div>
        </div>
        <div class="stat-item" class:is-disabled={!isDeviceActive}>
            <span class="icon">💻</span>
            <div class="stat-text">
                {#if isDeviceActive && cpuTemperature != null}
                    <span class="val">{cpuTemperature.toFixed(1)} <small>°C</small></span>
                    <span class="stat-tag">MCU Core</span>
                {:else}
                    <span class="val na">-- <small>°C</small></span>
                    <span class="stat-tag offline">Offline</span>
                {/if}
            </div>
        </div>
        <div class="stat-item" class:is-disabled={!isDeviceActive}>
            <span class="icon">⚡</span>
            <div class="stat-text">
                {#if isDeviceActive && cpuLoad != null}
                    <span class="val">{cpuLoad} <small>%</small></span>
                    <span class="stat-tag">Load</span>
                {:else}
                    <span class="val na">-- <small>%</small></span>
                    <span class="stat-tag offline">Offline</span>
                {/if}
            </div>
        </div>
    </div>

    <!-- Mini Telemetry Level Meters -->
    <div class="meters-container">
        <div class="meter-bar-item" class:meter-disabled={!isDeviceActive}>
            <div class="meter-labels">
                <span>ESP32-S3 Real Temperature</span>
                {#if isDeviceActive && displayRealTemp != null}
                    <strong>{displayRealTemp.toFixed(1)} °C</strong>
                {:else}
                    <strong class="na-label">Unavailable (Disconnected)</strong>
                {/if}
            </div>
            <div class="progress-track">
                <div class="progress-fill esp-temp" style={`width: ${isDeviceActive && displayRealTemp != null ? Math.min(100, Math.max(0, (displayRealTemp / 80) * 100)) : 0}%`}></div>
            </div>
        </div>

        <div class="meter-bar-item" class:meter-disabled={!isDeviceActive}>
            <div class="meter-labels">
                <span>Core CPU Utilization</span>
                {#if isDeviceActive && cpuLoad != null}
                    <strong>{cpuLoad} %</strong>
                {:else}
                    <strong class="na-label">Unavailable (Disconnected)</strong>
                {/if}
            </div>
            <div class="progress-track">
                <div class="progress-fill cpu" style={`width: ${isDeviceActive && cpuLoad != null ? Math.min(100, Math.max(0, cpuLoad)) : 0}%`}></div>
            </div>
        </div>
    </div>
</div>

<style>
    .env-card {
        background: #ffffff;
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
        gap: 8px;
        flex-wrap: wrap;
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
        background: #f1f5f9;
        color: #475569;
        padding: 2px 8px;
        border-radius: var(--radius-pill);
        border: 1px solid #e2e8f0;
    }

    .bus-badge.offline {
        background: #fef2f2;
        color: #dc2626;
        border-color: #fecaca;
    }

    .stat-item.is-disabled {
        opacity: 0.7;
    }

    .stat-item .val.na {
        color: var(--text-muted);
    }

    .stat-tag.offline {
        color: #dc2626;
    }

    .meter-bar-item.meter-disabled {
        opacity: 0.7;
    }

    .na-label {
        font-size: 0.72rem;
        color: #dc2626 !important;
        font-weight: 600;
    }

    .metrics-row {
        display: grid;
        grid-template-columns: repeat(3, 1fr);
        gap: 10px;
        background: #f8fafc;
        padding: 10px 14px;
        border-radius: var(--radius-md);
        border: 1px solid #e2e8f0;
    }

    .stat-item {
        display: flex;
        align-items: center;
        gap: 8px;
        min-width: 0;
    }

    .stat-item .icon {
        font-size: 0.95rem;
        flex-shrink: 0;
    }

    .stat-item .val {
        font-size: 0.88rem;
        font-weight: 700;
        color: var(--text-primary);
        font-family: var(--font-mono);
        white-space: nowrap;
    }

    .stat-item .val small {
        font-size: 0.68rem;
        color: var(--text-muted);
        font-weight: 500;
    }

    .stat-text {
        display: flex;
        flex-direction: column;
        min-width: 0;
    }

    .stat-tag {
        font-size: 0.62rem;
        color: var(--text-muted);
        text-transform: uppercase;
        font-weight: 700;
        letter-spacing: 0.02em;
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
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        font-size: 0.72rem;
        color: var(--text-secondary);
        font-weight: 500;
    }

    .meter-labels strong {
        color: var(--text-primary);
        font-family: var(--font-mono);
        font-weight: 700;
    }

    .progress-track {
        height: 6px;
        background: #e2e8f0;
        border-radius: var(--radius-pill);
        overflow: hidden;
    }

    .progress-fill {
        height: 100%;
        border-radius: var(--radius-pill);
        transition: width 0.4s ease;
    }

    .progress-fill.esp-temp {
        background: #d97706;
    }

    .progress-fill.cpu-temp {
        background: #3679a8;
    }

    .progress-fill.cpu {
        background: #16a34a;
    }

    @media (max-width: 420px) {
        .metrics-row {
            padding: 8px 10px;
            gap: 6px;
        }

        .stat-item .val {
            font-size: 0.78rem;
        }
    }
</style>
