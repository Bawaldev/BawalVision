<script lang="ts">
    interface Props {
        totalPlates?: number;
        passRate?: number;
        isOnline?: boolean;
        bufferedRecords?: number;
    }

    let { totalPlates = 100, passRate = 99.0, isOnline = true, bufferedRecords = 0 }: Props = $props();
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
        background: #ffffff;
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        padding: 14px;
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
        font-size: 0.85rem;
        font-weight: 700;
        color: var(--text-primary);
        line-height: 1.2;
    }

    .lot-pill, .ping-pill {
        font-size: 0.62rem;
        font-weight: 600;
        background: #f1f5f9;
        color: #475569;
        padding: 2px 7px;
        border-radius: var(--radius-pill);
        border: 1px solid #e2e8f0;
    }

    .stats-rows {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .stat-row {
        display: flex;
        justify-content: space-between;
        align-items: center;
        background: #f8fafc;
        padding: 6px 10px;
        border-radius: var(--radius-sm);
        border: 1px solid #e2e8f0;
    }

    .row-label {
        font-size: 0.7rem;
        color: var(--text-secondary);
        font-weight: 500;
    }

    .row-val {
        font-size: 0.82rem;
        font-family: var(--font-mono);
        color: var(--text-primary);
        font-weight: 700;
    }

    .row-val.green {
        color: #16a34a;
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
        background: #f8fafc;
        padding: 6px 10px;
        border-radius: var(--radius-sm);
        border: 1px solid #e2e8f0;
    }

    .status-circle {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #94a3b8;
    }

    .status-circle.active {
        background: #16a34a;
        box-shadow: 0 0 6px rgba(22, 163, 74, 0.5);
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
