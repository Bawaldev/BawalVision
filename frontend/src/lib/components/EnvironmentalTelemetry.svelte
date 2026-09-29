<script lang="ts">
    interface Props {
        temperature?: number;
        humidity?: number;
        pressure?: number;
        cpuLoad?: number;
    }

    let { temperature = 24.5, humidity = 65.0, pressure = 1013.25, cpuLoad = 18 }: Props = $props();
</script>

<div class="telemetry-panel-wrapper">
    <div class="panel-header">
        <h3>Physical Environmental Telemetry</h3>
        <span class="sensor-badge">I2C BUS 0x76</span>
    </div>

    <div class="telemetry-fields">
        <!-- Temperature -->
        <div class="field-row">
            <span class="field-label">Temperature</span>
            <div class="field-box">
                <span class="field-val">{temperature.toFixed(1)}</span>
                <span class="field-unit">C°</span>
            </div>
        </div>

        <!-- Humidity -->
        <div class="field-row">
            <span class="field-label">Humidity</span>
            <div class="field-box">
                <span class="field-val">{humidity.toFixed(1)}</span>
                <span class="field-unit">%Rh</span>
            </div>
        </div>

        <!-- Barometric Pressure -->
        <div class="field-row">
            <span class="field-label">Pressure</span>
            <div class="field-box">
                <span class="field-val">{pressure.toFixed(1)}</span>
                <span class="field-unit">hPa</span>
            </div>
        </div>

        <!-- CPU Load (Extra telemetry info) -->
        <div class="field-row">
            <span class="field-label">ESP32 Core Load</span>
            <div class="field-box">
                <span class="field-val">{cpuLoad}</span>
                <span class="field-unit">%</span>
            </div>
        </div>
    </div>
</div>

<style>
    .telemetry-panel-wrapper {
        background: var(--bg-panel, #121824);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
        padding: 12px 14px;
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .panel-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 6px;
        border-bottom: 1px solid #1e293b;
    }

    .panel-header h3 {
        margin: 0;
        font-size: 0.85rem;
        font-weight: 700;
        color: #f1f5f9;
        text-transform: uppercase;
        letter-spacing: 0.05em;
    }

    .sensor-badge {
        font-size: 0.6rem;
        font-family: monospace;
        color: #64748b;
        background: #080c14;
        padding: 2px 6px;
        border-radius: 4px;
        border: 1px solid #1e293b;
    }

    .telemetry-fields {
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .field-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .field-label {
        font-size: 0.8rem;
        font-weight: 600;
        color: #94a3b8;
    }

    .field-box {
        display: flex;
        align-items: center;
        justify-content: space-between;
        width: 110px;
        background: #080c14;
        border: 1px solid #1e293b;
        border-radius: 5px;
        padding: 6px 10px;
        box-shadow: inset 0 1px 3px rgba(0, 0, 0, 0.4);
    }

    .field-val {
        font-size: 0.95rem;
        font-family: monospace;
        font-weight: 800;
        color: #ffffff;
    }

    .field-unit {
        font-size: 0.75rem;
        font-weight: 700;
        color: #00f0ff;
        font-family: monospace;
    }
</style>
