<script lang="ts">
    interface Props {
        temperature?: number;
        isSpoiled?: boolean;
        tSafeHours?: number;
    }

    let { temperature = 24.5, isSpoiled = false, tSafeHours = 8.5 }: Props = $props();

    let freshnessIndex = $derived(
        isSpoiled ? 15 : Math.max(20, Math.min(100, 100 - Math.max(0, (temperature - 20) * 3)))
    );

    let statusText = $derived(
        isSpoiled || freshnessIndex < 40
            ? 'Spoiled / Reject'
            : freshnessIndex < 70
            ? 'Caution / Monitor'
            : 'Optimal & Fresh'
    );

    let statusClass = $derived(
        isSpoiled || freshnessIndex < 40
            ? 'danger'
            : freshnessIndex < 70
            ? 'warning'
            : 'fresh'
    );
</script>

<div class="spoilage-card">
    <div class="card-header">
        <div class="title-left">
            <span class="sensor-icon">🥗</span>
            <div>
                <h4>Food Quality & Freshness</h4>
                <p class="sub-label">Thermal Stability & Shelf Life</p>
            </div>
        </div>
        <span class="status-pill {statusClass}">{statusText}</span>
    </div>

    <!-- Lovelace Smooth Gradient Bar -->
    <div class="gradient-gauge-container">
        <div class="gauge-track">
            <div
                class="needle-pin"
                style={`left: ${freshnessIndex}%`}
            >
                <div class="pin-bubble">{freshnessIndex.toFixed(0)}%</div>
            </div>
        </div>
        <div class="gauge-labels">
            <span>Critical (0%)</span>
            <span>Warning (50%)</span>
            <span>Optimal (100%)</span>
        </div>
    </div>

    <div class="stats-tiles">
        <div class="tile">
            <span class="tile-title">Quality Score</span>
            <strong class="tile-val blue">{freshnessIndex.toFixed(0)} <small>/ 100</small></strong>
        </div>
        <div class="tile">
            <span class="tile-title">T-Safe Remaining</span>
            <strong class="tile-val green">{tSafeHours > 0 ? tSafeHours.toFixed(1) : '8.5'} <small>hours</small></strong>
        </div>
    </div>
</div>

<style>
    .spoilage-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        padding: 16px;
        display: flex;
        flex-direction: column;
        gap: 14px;
        box-shadow: var(--shadow-card);
    }

    .card-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .title-left {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .sensor-icon {
        font-size: 1.3rem;
    }

    .title-left h4 {
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

    .status-pill {
        font-size: 0.68rem;
        font-weight: 700;
        padding: 4px 10px;
        border-radius: var(--radius-pill);
    }

    .status-pill.fresh {
        background: var(--accent-green-dim);
        color: var(--accent-green);
        border: 1px solid rgba(34, 197, 94, 0.3);
    }

    .status-pill.warning {
        background: var(--accent-amber-dim);
        color: var(--accent-amber);
        border: 1px solid rgba(245, 158, 11, 0.3);
    }

    .status-pill.danger {
        background: var(--accent-red-dim);
        color: var(--accent-red);
        border: 1px solid rgba(244, 63, 94, 0.3);
    }

    .gradient-gauge-container {
        display: flex;
        flex-direction: column;
        gap: 8px;
        padding: 4px 0;
    }

    .gauge-track {
        position: relative;
        height: 10px;
        background: linear-gradient(90deg, #22c55e 0%, #eab308 50%, #f43f5e 100%);
        border-radius: var(--radius-pill);
    }

    .needle-pin {
        position: absolute;
        top: -6px;
        transform: translateX(-50%);
        display: flex;
        flex-direction: column;
        align-items: center;
        transition: left 0.4s ease;
    }

    .pin-bubble {
        font-size: 0.62rem;
        font-family: var(--font-mono);
        font-weight: 700;
        color: #ffffff;
        background: #0f172a;
        padding: 2px 6px;
        border-radius: var(--radius-pill);
        border: 1px solid #334155;
        box-shadow: 0 2px 6px rgba(0,0,0,0.4);
    }

    .gauge-labels {
        display: flex;
        justify-content: space-between;
        font-size: 0.68rem;
        color: var(--text-muted);
        font-weight: 500;
    }

    .stats-tiles {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 10px;
    }

    .tile {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        padding: 8px 12px;
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .tile-title {
        font-size: 0.65rem;
        color: var(--text-muted);
        font-weight: 600;
    }

    .tile-val {
        font-size: 0.95rem;
        font-family: var(--font-mono);
        font-weight: 700;
    }

    .tile-val.blue {
        color: var(--accent-blue);
    }

    .tile-val.green {
        color: var(--accent-green);
    }

    .tile-val small {
        font-size: 0.65rem;
        color: var(--text-muted);
        font-weight: normal;
    }
</style>
