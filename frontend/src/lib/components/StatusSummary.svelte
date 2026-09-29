<script lang="ts">
    interface Props {
        totalAccepted: number;
        totalRejected: number;
    }

    let { totalAccepted = 1248, totalRejected = 49 }: Props = $props();

    let total = $derived(totalAccepted + totalRejected);
    let passRate = $derived(total > 0 ? (totalAccepted / total) * 100 : 100);
</script>

<div class="status-panel-wrapper">
    <div class="panel-header">
        <h3>Status</h3>
        <span class="rate-indicator">{passRate.toFixed(1)}% PASS</span>
    </div>

    <div class="status-grid">
        <!-- Visual Acceptance Ring Gauge -->
        <div class="gauge-container">
            <svg viewBox="0 0 100 100" class="circle-gauge">
                <!-- Background Circle -->
                <circle cx="50" cy="50" r="40" fill="none" stroke="#1e293b" stroke-width="12" />
                <!-- Progress Arc -->
                <circle
                    cx="50"
                    cy="50"
                    r="40"
                    fill="none"
                    stroke="#10b981"
                    stroke-width="12"
                    stroke-dasharray="251.2"
                    stroke-dashoffset={251.2 * (1 - passRate / 100)}
                    stroke-linecap="round"
                    transform="rotate(-90 50 50)"
                />
            </svg>
            <div class="gauge-center-text">
                <strong>{passRate.toFixed(0)}%</strong>
                <small>YIELD</small>
            </div>
        </div>

        <!-- Accepted / Rejected Counts -->
        <div class="counters-col">
            <div class="status-row accepted">
                <div class="tag-box">Accepted</div>
                <div class="count-box">
                    <small>Total Accepted</small>
                    <strong>{totalAccepted.toLocaleString()}</strong>
                </div>
            </div>

            <div class="status-row rejected">
                <div class="tag-box reject">Rejected</div>
                <div class="count-box">
                    <small>Total Rejected</small>
                    <strong>{totalRejected.toLocaleString()}</strong>
                </div>
            </div>
        </div>
    </div>
</div>

<style>
    .status-panel-wrapper {
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

    .rate-indicator {
        font-size: 0.7rem;
        font-family: monospace;
        font-weight: 700;
        color: #10b981;
        background: rgba(16, 185, 129, 0.1);
        padding: 2px 6px;
        border-radius: 4px;
        border: 1px solid rgba(16, 185, 129, 0.3);
    }

    .status-grid {
        display: grid;
        grid-template-columns: 80px 1fr;
        gap: 14px;
        align-items: center;
    }

    .gauge-container {
        position: relative;
        width: 80px;
        height: 80px;
    }

    .circle-gauge {
        width: 100%;
        height: 100%;
    }

    .gauge-center-text {
        position: absolute;
        inset: 0;
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        text-align: center;
    }

    .gauge-center-text strong {
        font-size: 1.05rem;
        font-family: monospace;
        color: #ffffff;
        line-height: 1;
    }

    .gauge-center-text small {
        font-size: 0.55rem;
        font-weight: 700;
        color: #64748b;
        letter-spacing: 0.05em;
    }

    .counters-col {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .status-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        background: #0b0f19;
        border: 1px solid #1e293b;
        border-radius: 6px;
        padding: 6px 10px;
    }

    .tag-box {
        font-size: 0.7rem;
        font-weight: 700;
        padding: 3px 8px;
        border-radius: 4px;
        background: rgba(16, 185, 129, 0.2);
        color: #10b981;
        border: 1px solid rgba(16, 185, 129, 0.3);
    }

    .tag-box.reject {
        background: rgba(239, 68, 68, 0.2);
        color: #ef4444;
        border-color: rgba(239, 68, 68, 0.3);
    }

    .count-box {
        text-align: right;
    }

    .count-box small {
        display: block;
        font-size: 0.6rem;
        color: #64748b;
    }

    .count-box strong {
        font-size: 0.95rem;
        font-family: monospace;
        color: #f1f5f9;
    }
</style>
