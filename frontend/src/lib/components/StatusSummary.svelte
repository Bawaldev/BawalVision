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
                <circle cx="50" cy="50" r="40" fill="none" stroke="#2a3140" stroke-width="8" />
                {#if total > 0}
                    <circle
                        cx="50"
                        cy="50"
                        r="40"
                        fill="none"
                        stroke={passRate >= 80 ? '#22c55e' : '#f43f5e'}
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
</div>

<style>
    .summary-card {
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
        background: var(--accent-green-dim);
        color: var(--accent-green);
        padding: 3px 10px;
        border-radius: var(--radius-pill);
        border: 1px solid rgba(34, 197, 94, 0.3);
    }

    .yield-tag.standby {
        background: var(--bg-chip);
        color: var(--text-muted);
        border-color: var(--border-subtle);
    }

    .yield-tag.warn {
        background: var(--accent-red-dim);
        color: var(--accent-red);
        border-color: rgba(244, 63, 94, 0.3);
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
        font-size: 0.6rem;
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
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        padding: 8px 12px;
    }

    .counter-chip.pass {
        border-left: 3px solid var(--accent-green);
    }

    .counter-chip.fail {
        border-left: 3px solid var(--accent-red);
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
        background: var(--accent-green-dim);
        color: var(--accent-green);
    }

    .chip-circle.red {
        background: var(--accent-red-dim);
        color: var(--accent-red);
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
</style>
