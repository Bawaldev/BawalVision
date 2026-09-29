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
    <!-- Batch Summary (Wireframe: Batch Summary) -->
    <div class="summary-card">
        <div class="card-header">
            <h4>Batch Summary</h4>
            <span class="batch-id">BATCH #2026-A</span>
        </div>
        <div class="summary-rows">
            <div class="sum-row">
                <span class="sum-label">Total</span>
                <div class="sum-box">
                    <strong class="sum-val">{totalPlates.toLocaleString()}</strong>
                    <span class="sum-unit">Plate</span>
                </div>
            </div>

            <div class="sum-row">
                <span class="sum-label">Pass rate</span>
                <div class="sum-box">
                    <strong class="sum-val green">{passRate.toFixed(1)}</strong>
                    <span class="sum-unit">%</span>
                </div>
            </div>
        </div>
    </div>

    <!-- Cloud Sync Status (Wireframe: Cloud Sync Status) -->
    <div class="sync-card">
        <div class="card-header">
            <h4>Cloud Sync Status</h4>
            <span class="sync-ping">24ms AWS RDS</span>
        </div>
        <div class="sync-rows">
            <!-- Offline Buffer Status -->
            <div class="sync-item" class:active={bufferedRecords > 0}>
                <div class="check-box" class:checked={bufferedRecords > 0}>
                    {#if bufferedRecords > 0}✓{/if}
                </div>
                <div class="sync-info">
                    <span class="sync-label">Offline Buffer</span>
                    <small>{bufferedRecords} Records Queued</small>
                </div>
            </div>

            <!-- Online Sync Status -->
            <div class="sync-item" class:active={isOnline}>
                <div class="check-box" class:checked={isOnline}>
                    {#if isOnline}✓{/if}
                </div>
                <div class="sync-info">
                    <span class="sync-label">Online Sync</span>
                    <small>PostgreSQL RDS Connected</small>
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

    .summary-card, .sync-card {
        background: var(--bg-panel, #121824);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
        padding: 12px;
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .card-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 6px;
        border-bottom: 1px solid #1e293b;
    }

    .card-header h4 {
        margin: 0;
        font-size: 0.8rem;
        font-weight: 700;
        color: #f1f5f9;
        text-transform: uppercase;
        letter-spacing: 0.05em;
    }

    .batch-id, .sync-ping {
        font-size: 0.6rem;
        font-family: monospace;
        color: #64748b;
        background: #080c14;
        padding: 2px 5px;
        border-radius: 4px;
        border: 1px solid #1e293b;
    }

    .summary-rows, .sync-rows {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .sum-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .sum-label {
        font-size: 0.75rem;
        font-weight: 600;
        color: #94a3b8;
    }

    .sum-box {
        display: flex;
        align-items: center;
        gap: 6px;
        background: #080c14;
        border: 1px solid #1e293b;
        border-radius: 4px;
        padding: 4px 8px;
    }

    .sum-val {
        font-size: 0.85rem;
        font-family: monospace;
        color: #f8fafc;
    }

    .sum-val.green {
        color: #10b981;
    }

    .sum-unit {
        font-size: 0.65rem;
        font-weight: 700;
        color: #64748b;
    }

    .sync-item {
        display: flex;
        align-items: center;
        gap: 8px;
        background: #080c14;
        border: 1px solid #1e293b;
        border-radius: 5px;
        padding: 6px 8px;
    }

    .sync-item.active {
        border-color: rgba(0, 240, 255, 0.3);
        background: rgba(0, 240, 255, 0.04);
    }

    .check-box {
        width: 16px;
        height: 16px;
        border-radius: 3px;
        border: 1.5px solid #334155;
        display: flex;
        align-items: center;
        justify-content: center;
        font-size: 0.7rem;
        font-weight: 900;
        color: #000;
        background: transparent;
        flex-shrink: 0;
    }

    .check-box.checked {
        background: #00f0ff;
        border-color: #00f0ff;
    }

    .sync-info {
        display: flex;
        flex-direction: column;
    }

    .sync-label {
        font-size: 0.75rem;
        font-weight: 700;
        color: #cbd5e1;
        line-height: 1.1;
    }

    .sync-info small {
        font-size: 0.6rem;
        color: #64748b;
    }
</style>
