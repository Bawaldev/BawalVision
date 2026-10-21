<script lang="ts">
    import type { SceneOpticalMetrics } from '../types';

    interface Props {
        isConnected?: boolean;
        isMock?: boolean;
        cpuTemperature?: number | null;
        temperature?: number | null;
        cpuLoad?: number | null;
        firmwareVersion?: string;
        sceneOptical?: SceneOpticalMetrics;
    }

    let {
        isConnected = false,
        isMock = false,
        cpuTemperature = null,
        temperature = null,
        cpuLoad = null,
        firmwareVersion = '1.0.4',
        sceneOptical = undefined
    }: Props = $props();

    // ESP32-S3 real temperature, CPU temp, and CPU utility are available if connected or simulated in mock mode
    let isDeviceActive = $derived(
        (Boolean(isConnected) && temperature != null && temperature > 0) || isMock
    );

    let displayRealTemp = $derived(
        temperature != null ? temperature : (isMock ? 37.2 : null)
    );

    let displayCpuLoad = $derived(
        cpuLoad != null ? cpuLoad : (isMock ? 18 : null)
    );
</script>

<div class="env-card">
    <div class="card-top">
        <div class="header-left">
            <span class="room-icon">🌡️</span>
            <div>
                <h4>Thermal & Hardware Telemetry</h4>
                <p class="sub-label">
                    {#if isMock}
                        Simulated Node Telemetry (MOCK_VIDEO_CAM)
                    {:else if isDeviceActive}
                        ESP-IDF Live ESP32-S3 Real Temperature
                    {:else}
                        ESP-IDF Device Disconnected
                    {/if}
                </p>
            </div>
        </div>
        <span class="bus-badge" class:offline={!isDeviceActive && !isMock} class:mock={isMock}>
            {#if isMock}
                Mock Sim
            {:else if isDeviceActive}
                v{firmwareVersion}
            {:else}
                Offline
            {/if}
        </span>
    </div>

    <!-- Horizontal IoT Metrics Strip -->
    <div class="metrics-row">
        <div class="stat-item" class:is-disabled={!isDeviceActive}>
            <span class="icon">🌡️</span>
            <div class="stat-text">
                {#if isDeviceActive && displayRealTemp != null}
                    <span class="val">{displayRealTemp.toFixed(1)} <small>°C</small></span>
                    <span class="stat-tag">{isMock ? 'Node Temp (Sim)' : 'ESP32-S3 Temp'}</span>
                {:else}
                    <span class="val na">-- <small>°C</small></span>
                    <span class="stat-tag offline">Offline</span>
                {/if}
            </div>
        </div>
        <div class="stat-item" class:is-disabled={!isDeviceActive}>
            <span class="icon">⚡</span>
            <div class="stat-text">
                {#if isDeviceActive && displayCpuLoad != null}
                    <span class="val">{displayCpuLoad} <small>%</small></span>
                    <span class="stat-tag">{isMock ? 'Vision Load (Sim)' : 'CPU Load'}</span>
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
                <span>{isMock ? 'Simulated Node Temperature' : 'ESP32-S3 Temperature'}</span>
                {#if isDeviceActive && displayRealTemp != null}
                    <strong>{displayRealTemp.toFixed(1)} °C {isMock ? '(Sim)' : ''}</strong>
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
                <span>{isMock ? 'Vision Engine Load' : 'Core CPU Utilization'}</span>
                {#if isDeviceActive && displayCpuLoad != null}
                    <strong>{displayCpuLoad} % {isMock ? '(Sim)' : ''}</strong>
                {:else}
                    <strong class="na-label">Unavailable (Disconnected)</strong>
                {/if}
            </div>
            <div class="progress-track">
                <div class="progress-fill cpu" style={`width: ${isDeviceActive && displayCpuLoad != null ? Math.min(100, Math.max(0, displayCpuLoad)) : 0}%`}></div>
            </div>
        </div>

        {#if sceneOptical && sceneOptical.lux_luminance != null && sceneOptical.lux_luminance > 0}
            <div class="meter-bar-item">
                <div class="meter-labels">
                    <span>Ambient Illumination (AVX2 Lux)</span>
                    <strong>{sceneOptical.lux_luminance.toFixed(0)} lx • {sceneOptical.white_balance_status} ({sceneOptical.color_temperature}K)</strong>
                </div>
                <div class="progress-track">
                    <div class="progress-fill optical" style={`width: ${Math.min(100, (sceneOptical.lux_luminance / 255) * 100)}%`}></div>
                </div>
            </div>
        {/if}
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

    .bus-badge.mock {
        background: #fef3c7;
        color: #d97706;
        border-color: #fde68a;
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
        grid-template-columns: repeat(2, 1fr);
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

    .progress-fill.optical {
        background: linear-gradient(90deg, #f59e0b, #eab308);
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
