<script lang="ts">
    interface Props {
        temperature?: number;
        isSpoiled?: boolean;
        tSafeHours?: number;
        hasFood?: boolean;
        qualityScore?: number;
    }

    let { temperature = 24.5, isSpoiled = false, tSafeHours = 0, hasFood = false, qualityScore = 0 }: Props = $props();

    let freshnessIndex = $derived(
        !hasFood
            ? 0
            : isSpoiled
            ? 15
            : qualityScore > 0
            ? Math.max(10, Math.min(100, qualityScore - (temperature > 35 ? (temperature - 35) * 2.5 : 0)))
            : Math.max(20, Math.min(100, 100 - Math.max(0, (temperature - 20) * 2)))
    );

    let statusText = $derived(
        !hasFood
            ? 'Awaiting Food Scan'
            : isSpoiled || freshnessIndex < 40
            ? 'Spoiled / Reject'
            : freshnessIndex < 70
            ? 'Caution / Review'
            : 'Optimal & Fresh'
    );

    let statusClass = $derived(
        !hasFood
            ? 'standby'
            : isSpoiled || freshnessIndex < 40
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

    <!-- Smooth Gradient Bar with Pin Indicator -->
    <div class="gradient-gauge-container">
        <div class="gauge-track">
            <div
                class="needle-pin"
                style={`left: ${hasFood ? freshnessIndex : 0}%`}
            >
                <div class="pin-bubble">{hasFood ? `${freshnessIndex.toFixed(0)}%` : '0%'}</div>
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
            <strong class="tile-val blue">{hasFood ? freshnessIndex.toFixed(0) : '0'} <small>/ 100</small></strong>
        </div>
        <div class="tile">
            <span class="tile-title">T-Safe Remaining</span>
            <strong class="tile-val green">{hasFood && tSafeHours > 0 ? tSafeHours.toFixed(1) : '8.5'} <small>hours</small></strong>
        </div>
    </div>
</div>

<style>
    .spoilage-card {
        background: #ffffff;
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
        padding: 3px 10px;
        border-radius: var(--radius-pill);
    }

    .status-pill.fresh {
        background: #dcfce7;
        color: #16a34a;
        border: 1px solid #bbf7d0;
    }

    .status-pill.standby {
        background: #f1f5f9;
        color: #64748b;
        border: 1px solid #e2e8f0;
    }

    .status-pill.warning {
        background: #fef3c7;
        color: #d97706;
        border: 1px solid #fde68a;
    }

    .status-pill.danger {
        background: #fee2e2;
        color: #dc2626;
        border: 1px solid #fecaca;
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
        background: linear-gradient(90deg, #dc2626 0%, #d97706 50%, #16a34a 100%);
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
        background: #1e293b;
        padding: 2px 6px;
        border-radius: var(--radius-pill);
        border: 1px solid #475569;
        box-shadow: 0 1px 3px rgba(0, 0, 0, 0.2);
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
        background: #f8fafc;
        border: 1px solid #e2e8f0;
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
        color: #3679a8;
    }

    .tile-val.green {
        color: #16a34a;
    }

    .tile-val small {
        font-size: 0.65rem;
        color: var(--text-muted);
        font-weight: normal;
    }
</style>
