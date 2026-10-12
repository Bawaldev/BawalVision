<script lang="ts">
    interface Props {
        totalAccepted: number;
        totalRejected: number;
    }

    let { totalAccepted = 0, totalRejected = 0 }: Props = $props();

    let total = $derived(totalAccepted + totalRejected);
    let passRate = $derived(total > 0 ? (totalAccepted / total) * 100 : 0);
</script>

<div class="summary-card">
    <div class="card-header">
        <div class="header-left">
            <span class="card-icon">📊</span>
            <div>
                <h4>QC Inspection Yield</h4>
                <p class="sub-label">Production Statistics</p>
            </div>
        </div>
        {#if total > 0}
            <span class="yield-tag" class:warn={passRate < 80}>{passRate.toFixed(1)}% Pass</span>
        {:else}
            <span class="yield-tag standby">Awaiting Scan</span>
        {/if}
    </div>

    <div class="gauge-and-counters">
        <!-- Circular Progress Ring Gauge -->
        <div class="gauge-box">
            <svg viewBox="0 0 100 100" class="ring-svg">
                <!-- Inactive Track -->
                <circle cx="50" cy="50" r="40" fill="none" stroke="#e2e8f0" stroke-width="8" />
                {#if total > 0}
                    <circle
                        cx="50"
                        cy="50"
                        r="40"
                        fill="none"
                        stroke={passRate >= 80 ? '#16a34a' : '#dc2626'}
                        stroke-width="8"
                        stroke-dasharray="251.2"
                        stroke-dashoffset={251.2 * (1 - passRate / 100)}
                        stroke-linecap="round"
                        transform="rotate(-90 50 50)"
                    />
                {/if}
            </svg>
            <div class="gauge-center">
                <strong>{total > 0 ? `${passRate.toFixed(0)}%` : '0%'}</strong>
                <small>{total > 0 ? 'Yield' : 'Standby'}</small>
            </div>
        </div>

        <!-- Entity Counter Chips -->
        <div class="counters-list">
            <div class="counter-chip pass">
                <div class="chip-circle green">✓</div>
                <div class="counter-info">
                    <span class="count-label">Accepted</span>
                    <strong class="count-num">{totalAccepted.toLocaleString()}</strong>
                </div>
            </div>

            <div class="counter-chip fail">
                <div class="chip-circle red">✕</div>
                <div class="counter-info">
                    <span class="count-label">Rejected</span>
                    <strong class="count-num">{totalRejected.toLocaleString()}</strong>
                </div>
            </div>
        </div>
    </div>

    <!-- Production Metrics Footer -->
    <div class="summary-footer-stats">
        <div class="footer-stat">
            <span class="stat-k">Total Tested</span>
            <strong class="stat-v">{total.toLocaleString()} Trays</strong>
        </div>
        <div class="footer-stat">
            <span class="stat-k">Yield Status</span>
            <strong class="stat-v" class:green={passRate >= 80} class:red={total > 0 && passRate < 80}>
                {total === 0 ? 'Standby' : (passRate >= 80 ? 'Optimal' : 'Attention')}
            </strong>
        </div>
    </div>
</div>

<style>
    .summary-card {
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

    .header-left {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .card-icon {
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

    .yield-tag {
        font-size: 0.7rem;
        font-weight: 700;
        background: #dcfce7;
        color: #16a34a;
        padding: 3px 10px;
        border-radius: var(--radius-pill);
        border: 1px solid #bbf7d0;
    }

    .yield-tag.standby {
        background: #f1f5f9;
        color: #64748b;
        border-color: #e2e8f0;
    }

    .yield-tag.warn {
        background: #fee2e2;
        color: #dc2626;
        border-color: #fecaca;
    }

    .gauge-and-counters {
        display: grid;
        grid-template-columns: 80px 1fr;
        gap: 14px;
        align-items: center;
    }

    .gauge-box {
        position: relative;
        width: 80px;
        height: 80px;
    }

    .ring-svg {
        width: 100%;
        height: 100%;
    }

    .gauge-center {
        position: absolute;
        inset: 0;
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
    }

    .gauge-center strong {
        font-size: 1.1rem;
        font-weight: 800;
        color: var(--text-primary);
        line-height: 1;
    }

    .gauge-center small {
        font-size: 0.62rem;
        color: var(--text-muted);
        font-weight: 600;
    }

    .counters-list {
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .counter-chip {
        display: flex;
        align-items: center;
        gap: 10px;
        background: #f8fafc;
        border: 1px solid #e2e8f0;
        border-radius: var(--radius-md);
        padding: 8px 12px;
    }

    .counter-chip.pass {
        border-left: 3px solid #16a34a;
    }

    .counter-chip.fail {
        border-left: 3px solid #dc2626;
    }

    .chip-circle {
        width: 24px;
        height: 24px;
        border-radius: 50%;
        display: flex;
        align-items: center;
        justify-content: center;
        font-size: 0.75rem;
        font-weight: 800;
        flex-shrink: 0;
    }

    .chip-circle.green {
        background: #dcfce7;
        color: #16a34a;
    }

    .chip-circle.red {
        background: #fee2e2;
        color: #dc2626;
    }

    .counter-info {
        display: flex;
        justify-content: space-between;
        align-items: baseline;
        flex: 1;
    }

    .count-label {
        font-size: 0.72rem;
        color: var(--text-secondary);
        font-weight: 600;
    }

    .count-num {
        font-size: 0.95rem;
        font-family: var(--font-mono);
        color: var(--text-primary);
        font-weight: 700;
    }

    .summary-footer-stats {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 8px;
        padding-top: 10px;
        border-top: 1px solid #e2e8f0;
    }

    .footer-stat {
        display: flex;
        flex-direction: column;
        gap: 2px;
        background: #f8fafc;
        padding: 6px 10px;
        border-radius: var(--radius-sm);
        border: 1px solid #e2e8f0;
    }

    .stat-k {
        font-size: 0.62rem;
        color: var(--text-muted);
        text-transform: uppercase;
        letter-spacing: 0.03em;
        font-weight: 600;
    }

    .stat-v {
        font-size: 0.78rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .stat-v.green {
        color: #16a34a;
    }

    .stat-v.red {
        color: #dc2626;
    }
</style>
