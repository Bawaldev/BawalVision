<script lang="ts">
    import type { InferenceLogEntry } from '../types';

    interface Props {
        logs: InferenceLogEntry[];
        onClear?: () => void;
    }

    let { logs = [], onClear }: Props = $props();

    let autoScroll = $state(true);
    let logBoxEl = $state<HTMLDivElement | null>(null);

    $effect(() => {
        if (autoScroll && logBoxEl) {
            logBoxEl.scrollTop = logBoxEl.scrollHeight;
        }
    });
</script>

<div class="coperion-card">
    <div class="card-header">
        <div class="header-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" class="card-header-icon">
                <path stroke-linecap="round" stroke-linejoin="round" d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z" />
            </svg>
            <h4>LINE AUDIT & EVENT LOG</h4>
        </div>
        <div class="header-actions">
            <button
                type="button"
                class="ctrl-btn"
                class:active={autoScroll}
                onclick={() => (autoScroll = !autoScroll)}
            >
                <span class="btn-dot" class:on={autoScroll}></span>
                {autoScroll ? 'Autoscroll On' : 'Autoscroll Off'}
            </button>
            <button type="button" class="ctrl-btn" onclick={onClear}>
                Clear
            </button>
        </div>
    </div>

    <div class="log-box" bind:this={logBoxEl}>
        {#if logs.length === 0}
            <div class="empty-log">
                <span>[LOG STANDBY] No active inspection or telemetry events yet.</span>
            </div>
        {:else}
            {#each logs as log (log.id)}
                <div class="log-line {log.level.toLowerCase()}">
                    <span class="timestamp">[{log.timestamp}]</span>
                    <span class="level-tag {log.level.toLowerCase()}">
                        [{log.level === 'SUCCESS' ? 'OK' : log.level === 'ERROR' ? 'NOK' : log.level}]
                    </span>
                    <span class="message-body">{log.message}</span>
                    {#if log.latencyMs}
                        <span class="latency-badge">{log.latencyMs.toFixed(1)}ms</span>
                    {/if}
                </div>
            {/each}
        {/if}
    </div>
</div>

<style>
    .coperion-card {
        background: #ffffff;
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        padding: 14px 16px;
        box-shadow: var(--shadow-card);
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .card-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 8px;
        border-bottom: 1px solid #edf2f7;
    }

    .header-title {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .card-header-icon {
        width: 15px;
        height: 15px;
        color: #64748b;
    }

    .header-title h4 {
        margin: 0;
        font-size: 0.75rem;
        font-weight: 700;
        letter-spacing: 0.05em;
        text-transform: uppercase;
        color: #1e293b;
    }

    .header-actions {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .ctrl-btn {
        background: #f1f5f9;
        border: 1px solid #e2e8f0;
        color: #475569;
        font-size: 0.65rem;
        font-weight: 600;
        padding: 3px 8px;
        border-radius: 3px;
        cursor: pointer;
        display: flex;
        align-items: center;
        gap: 5px;
        transition: all 0.12s ease;
    }

    .ctrl-btn:hover {
        background: #e2e8f0;
        color: #1e293b;
    }

    .ctrl-btn.active {
        background: #e8f2f9;
        border-color: #3679a8;
        color: #3679a8;
    }

    .btn-dot {
        width: 5px;
        height: 5px;
        border-radius: 50%;
        background: #94a3b8;
    }

    .btn-dot.on {
        background: #3679a8;
    }

    .log-box {
        height: 120px;
        background: #f8fafc;
        border: 1px solid #e2e8f0;
        border-radius: 4px;
        padding: 8px 10px;
        overflow-y: auto;
        font-family: var(--font-mono);
        font-size: 0.72rem;
        line-height: 1.5;
        display: flex;
        flex-direction: column;
        gap: 4px;
    }

    .log-line {
        display: flex;
        align-items: baseline;
        gap: 6px;
        color: #334155;
        word-break: break-all;
    }

    .timestamp {
        color: #94a3b8;
        flex-shrink: 0;
        font-size: 0.68rem;
    }

    .level-tag {
        font-weight: 700;
        flex-shrink: 0;
        font-size: 0.65rem;
        padding: 0 4px;
        border-radius: 2px;
    }

    .level-tag.info {
        color: #0284c7;
        background: #e0f2fe;
    }

    .level-tag.warn {
        color: #d97706;
        background: #fef3c7;
    }

    .level-tag.error {
        color: #dc2626;
        background: #fee2e2;
    }

    .level-tag.success {
        color: #16a34a;
        background: #dcfce7;
    }

    .message-body {
        flex: 1;
        color: #1e293b;
    }

    .latency-badge {
        font-size: 0.62rem;
        color: #7c3aed;
        background: #f3e8ff;
        padding: 0 4px;
        border-radius: 2px;
        flex-shrink: 0;
    }

    .empty-log {
        color: #94a3b8;
        padding: 6px 0;
        font-size: 0.72rem;
    }
</style>
