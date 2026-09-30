<script lang="ts">
    interface Props {
        totalPlates?: number;
        passRate?: number;
        isOnline?: boolean;
        bufferedRecords?: number;
    }

    let { totalPlates = 1297, passRate = 96.2, isOnline = true, bufferedRecords = 0 }: Props = $props();
</script>

<div class="batch-cloud-grid">
    <!-- Batch Production Summary Card -->
    <div class="entity-card">
        <div class="card-header">
            <div class="header-left">
                <span class="icon">📦</span>
                <h4>Batch Summary</h4>
            </div>
            <span class="lot-pill">Lot #2026-A</span>
        </div>
        <div class="stats-rows">
            <div class="stat-row">
                <span class="row-label">Total Inspected</span>
                <strong class="row-val">{totalPlates.toLocaleString()} <small>units</small></strong>
            </div>
            <div class="stat-row">
                <span class="row-label">Pass Rate</span>
                <strong class="row-val green">{passRate.toFixed(1)}%</strong>
            </div>
        </div>
    </div>

    <!-- Telemetry Cloud Sync Status Card -->
    <div class="entity-card">
        <div class="card-header">
            <div class="header-left">
                <span class="icon">☁️</span>
                <h4>Database Sync</h4>
            </div>
            <span class="ping-pill">24ms AWS RDS</span>
        </div>
        <div class="sync-items-list">
            <div class="sync-item">
                <div class="status-circle" class:active={bufferedRecords > 0}></div>
                <div class="sync-info">
                    <span class="sync-title">Local Buffer</span>
                    <small>{bufferedRecords} queued</small>
                </div>
            </div>

            <div class="sync-item">
                <div class="status-circle" class:active={isOnline}></div>
                <div class="sync-info">
                    <span class="sync-title">PostgreSQL RDS</span>
                    <small>{isOnline ? 'Connected' : 'Offline'}</small>
                </div>
            </div>
        </div>
    </div>
</div>

<style>
    .batch-cloud-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 12px;
    }

    .entity-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        padding: 16px;
        display: flex;
        flex-direction: column;
        gap: 12px;
        box-shadow: var(--shadow-card);
    }

    .card-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .header-left {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .header-left .icon {
        font-size: 1.1rem;
    }

    .header-left h4 {
        margin: 0;
        font-size: 0.88rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .lot-pill, .ping-pill {
        font-size: 0.65rem;
        font-weight: 600;
        background: var(--bg-chip);
        color: var(--text-secondary);
        padding: 2px 8px;
        border-radius: var(--radius-pill);
    }

    .stats-rows {
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .stat-row {
        display: flex;
        justify-content: space-between;
        align-items: center;
        background: var(--bg-inset);
        padding: 6px 10px;
        border-radius: var(--radius-sm);
    }

    .row-label {
        font-size: 0.72rem;
        color: var(--text-secondary);
    }

    .row-val {
        font-size: 0.85rem;
        font-family: var(--font-mono);
        color: var(--text-primary);
    }

    .row-val.green {
        color: var(--accent-green);
    }

    .row-val small {
        font-size: 0.65rem;
        color: var(--text-muted);
        font-weight: normal;
    }

    .sync-items-list {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .sync-item {
        display: flex;
        align-items: center;
        gap: 8px;
        background: var(--bg-inset);
        padding: 6px 10px;
        border-radius: var(--radius-sm);
    }

    .status-circle {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: var(--text-muted);
    }

    .status-circle.active {
        background: var(--accent-blue);
        box-shadow: 0 0 6px var(--accent-blue);
    }

    .sync-info {
        display: flex;
        flex-direction: column;
    }

    .sync-title {
        font-size: 0.72rem;
        font-weight: 600;
        color: var(--text-primary);
        line-height: 1.1;
    }

    .sync-info small {
        font-size: 0.62rem;
        color: var(--text-muted);
    }
</style>
