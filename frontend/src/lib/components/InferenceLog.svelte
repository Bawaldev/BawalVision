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

<div class="log-card">
    <div class="card-header">
        <div class="header-left">
            <span class="card-icon">📋</span>
            <div>
                <h4>System & Inspection Events</h4>
                <p class="sub-label">Live Event Stream</p>
            </div>
        </div>
        <div class="header-actions">
            <button
                type="button"
                class="chip-btn"
                class:active={autoScroll}
                onclick={() => (autoScroll = !autoScroll)}
            >
                {autoScroll ? 'Autoscroll: On' : 'Autoscroll: Off'}
            </button>
            <button type="button" class="chip-btn" onclick={onClear}>
                Clear
            </button>
        </div>
    </div>

    <div class="log-box" bind:this={logBoxEl}>
        {#if logs.length === 0}
            <div class="empty-log">
                <span>Standby: Awaiting live inspection and telemetry events...</span>
            </div>
        {:else}
            {#each logs as log (log.id)}
                <div class="log-line {log.level.toLowerCase()}">
                    <span class="timestamp">[{log.timestamp}]</span>
                    <span class="level-tag {log.level.toLowerCase()}">{log.level}</span>
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
    .log-card {
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

    .header-actions {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .chip-btn {
        background: var(--bg-chip);
        border: 1px solid var(--border-subtle);
        color: var(--text-secondary);
        font-size: 0.68rem;
        font-weight: 600;
        padding: 3px 10px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        transition: all 0.12s ease;
    }

    .chip-btn:hover {
        background: var(--bg-card-hover);
        color: var(--text-primary);
    }

    .chip-btn.active {
        background: var(--accent-blue-dim);
        border-color: rgba(56, 189, 248, 0.4);
        color: var(--accent-blue);
    }

    .log-box {
        height: 120px;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        padding: 10px 12px;
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
        gap: 8px;
        color: var(--text-secondary);
        word-break: break-all;
    }

    .timestamp {
        color: var(--text-muted);
        flex-shrink: 0;
    }

    .level-tag {
        font-weight: 700;
        flex-shrink: 0;
        font-size: 0.65rem;
        padding: 1px 6px;
        border-radius: var(--radius-pill);
    }

    .level-tag.info {
        color: var(--accent-blue);
        background: var(--accent-blue-dim);
    }

    .level-tag.warn {
        color: var(--accent-amber);
        background: var(--accent-amber-dim);
    }

    .level-tag.error {
        color: var(--accent-red);
        background: var(--accent-red-dim);
    }

    .level-tag.success {
        color: var(--accent-green);
        background: var(--accent-green-dim);
    }

    .message-body {
        flex: 1;
        color: var(--text-primary);
    }

    .latency-badge {
        font-size: 0.62rem;
        background: var(--accent-purple-dim);
        color: var(--accent-purple);
        padding: 1px 6px;
        border-radius: var(--radius-pill);
        flex-shrink: 0;
    }

    .empty-log {
        color: var(--text-muted);
        padding: 8px 0;
        font-style: italic;
    }
</style>
