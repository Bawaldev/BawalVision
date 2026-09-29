<script lang="ts">
    interface Props {
        gasVoc?: number;
        isSpoiled?: boolean;
        tSafeHours?: number;
    }

    let { gasVoc = 110.5, isSpoiled = false, tSafeHours = 8.5 }: Props = $props();

    // Spoilage calculation based on VOC gas resistance (kOhm):
    // > 80 kOhm: High resistance = Low VOC emission = Fresh food (0 - 30% spoilage index)
    // 40 - 80 kOhm: Moderate VOC = Caution (30 - 70%)
    // < 40 kOhm: High VOC = Spoiled (> 70%)
    let spoilagePercentage = $derived(
        Math.min(100, Math.max(0, 100 - (gasVoc / 150) * 100))
    );

    let statusText = $derived(
        isSpoiled || spoilagePercentage > 70
            ? 'SPOILED / DANGER'
            : spoilagePercentage > 35
            ? 'CAUTION / EXPIRING'
            : 'FRESH & SAFE'
    );

    let statusClass = $derived(
        isSpoiled || spoilagePercentage > 70
            ? 'danger'
            : spoilagePercentage > 35
            ? 'warning'
            : 'fresh'
    );
</script>

<div class="spoilage-panel-wrapper">
    <div class="panel-header">
        <div class="title-group">
            <h3>Chemical Spoilage Bar</h3>
            <span class="sub-tag">BME680 VOC SENSOR</span>
        </div>
        <span class="spoilage-state-badge {statusClass}">{statusText}</span>
    </div>

    <!-- Visual Linear Spoilage Bar (Wireframe: Chemical Spoilage Bar) -->
    <div class="bar-container">
        <div class="gradient-track">
            <!-- Animated Needle Indicator -->
            <div
                class="needle-pointer"
                style={`left: ${spoilagePercentage}%`}
            >
                <div class="needle-tip"></div>
                <div class="needle-val">{spoilagePercentage.toFixed(0)}%</div>
            </div>
        </div>
        <div class="scale-labels">
            <span>0% (FRESH)</span>
            <span>50% (MODERATE)</span>
            <span>100% (SPOILED)</span>
        </div>
    </div>

    <div class="metrics-row">
        <div class="metric-pill">
            <span class="label">GAS RESISTANCE</span>
            <strong class="val cyan">{gasVoc.toFixed(1)} kΩ</strong>
        </div>
        <div class="metric-pill">
            <span class="label">T-SAFE PREDICTION</span>
            <strong class="val green">{tSafeHours.toFixed(1)} Hours</strong>
        </div>
    </div>
</div>

<style>
    .spoilage-panel-wrapper {
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

    .title-group h3 {
        margin: 0;
        font-size: 0.85rem;
        font-weight: 700;
        color: #f1f5f9;
        text-transform: uppercase;
        letter-spacing: 0.05em;
    }

    .sub-tag {
        font-size: 0.6rem;
        color: #64748b;
        letter-spacing: 0.05em;
        font-family: monospace;
    }

    .spoilage-state-badge {
        font-size: 0.65rem;
        font-weight: 800;
        padding: 3px 8px;
        border-radius: 4px;
        letter-spacing: 0.05em;
    }

    .spoilage-state-badge.fresh {
        background: rgba(16, 185, 129, 0.2);
        color: #10b981;
        border: 1px solid rgba(16, 185, 129, 0.4);
    }

    .spoilage-state-badge.warning {
        background: rgba(245, 158, 11, 0.2);
        color: #f59e0b;
        border: 1px solid rgba(245, 158, 11, 0.4);
    }

    .spoilage-state-badge.danger {
        background: rgba(239, 68, 68, 0.2);
        color: #ef4444;
        border: 1px solid rgba(239, 68, 68, 0.4);
    }

    .bar-container {
        display: flex;
        flex-direction: column;
        gap: 6px;
        padding: 6px 0;
    }

    .gradient-track {
        position: relative;
        height: 14px;
        background: linear-gradient(90deg, #10b981 0%, #f59e0b 50%, #ef4444 100%);
        border-radius: 7px;
        box-shadow: inset 0 2px 4px rgba(0, 0, 0, 0.5);
    }

    .needle-pointer {
        position: absolute;
        top: -6px;
        transform: translateX(-50%);
        display: flex;
        flex-direction: column;
        align-items: center;
        transition: left 0.4s ease;
    }

    .needle-tip {
        width: 4px;
        height: 26px;
        background: #ffffff;
        border-radius: 2px;
        box-shadow: 0 0 8px rgba(0, 0, 0, 0.8), 0 0 4px #00f0ff;
    }

    .needle-val {
        font-size: 0.6rem;
        font-family: monospace;
        font-weight: 800;
        color: #ffffff;
        background: #0b0f19;
        padding: 1px 4px;
        border-radius: 3px;
        border: 1px solid #334155;
        margin-top: 2px;
    }

    .scale-labels {
        display: flex;
        justify-content: space-between;
        font-size: 0.6rem;
        color: #64748b;
        font-family: monospace;
        font-weight: 600;
    }

    .metrics-row {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 8px;
    }

    .metric-pill {
        background: #0b0f19;
        border: 1px solid #1e293b;
        border-radius: 6px;
        padding: 6px 10px;
        display: flex;
        flex-direction: column;
    }

    .metric-pill .label {
        font-size: 0.6rem;
        color: #64748b;
        font-weight: 700;
    }

    .metric-pill .val {
        font-size: 0.9rem;
        font-family: monospace;
    }

    .metric-pill .val.cyan {
        color: #00f0ff;
    }

    .metric-pill .val.green {
        color: #10b981;
    }
</style>
