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

<div class="inference-log-wrapper">
    <div class="log-header">
        <div class="title-meta">
            <span class="terminal-dot green"></span>
            <h3>Inference log</h3>
            <span class="stream-status">STREAM ACTIVE (100 Hz Ingress)</span>
        </div>
        <div class="log-actions">
            <button
                type="button"
                class="action-btn"
                class:active={autoScroll}
                onclick={() => (autoScroll = !autoScroll)}
            >
                {autoScroll ? 'AUTOSCROLL: ON' : 'AUTOSCROLL: OFF'}
            </button>
            <button type="button" class="action-btn" onclick={onClear}>
                CLEAR
            </button>
        </div>
    </div>

    <div class="log-box" bind:this={logBoxEl}>
        {#if logs.length === 0}
            <div class="empty-log">
                <span>[LOG] Waiting for ESP32 camera frames and AI1 inference triggers...</span>
            </div>
        {:else}
            {#each logs as log (log.id)}
                <div class="log-line {log.level.toLowerCase()}">
                    <span class="timestamp">[{log.timestamp}]</span>
                    <span class="level-tag {log.level.toLowerCase()}">[{log.level}]</span>
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
    .inference-log-wrapper {
        background: var(--bg-panel, #0e131f);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
        padding: 12px;
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .log-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 8px;
        border-bottom: 1px solid #1e293b;
    }

    .title-meta {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .terminal-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #10b981;
        box-shadow: 0 0 6px #10b981;
    }

    .title-meta h3 {
        margin: 0;
        font-size: 0.85rem;
        font-weight: 700;
        color: #f1f5f9;
        font-family: monospace;
    }

    .stream-status {
        font-size: 0.65rem;
        font-family: monospace;
        color: #64748b;
        background: #080c14;
        padding: 2px 6px;
        border-radius: 4px;
        border: 1px solid #1e293b;
    }

    .log-actions {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .action-btn {
        background: #141b2d;
        border: 1px solid #23304a;
        color: #94a3b8;
        font-size: 0.65rem;
        font-family: monospace;
        font-weight: 700;
        padding: 4px 8px;
        border-radius: 4px;
        cursor: pointer;
        transition: all 0.2s ease;
    }

    .action-btn:hover {
        background: #1e293b;
        color: #f1f5f9;
    }

    .action-btn.active {
        background: rgba(0, 240, 255, 0.15);
        border-color: rgba(0, 240, 255, 0.4);
        color: #00f0ff;
    }

    .log-box {
        height: 120px;
        background: #070a10;
        border: 1px solid #141b2d;
        border-radius: 4px;
        padding: 8px 10px;
        overflow-y: auto;
        font-family: monospace;
        font-size: 0.75rem;
        line-height: 1.4;
        display: flex;
        flex-direction: column;
        gap: 3px;
    }

    .log-line {
        display: flex;
        align-items: baseline;
        gap: 6px;
        color: #94a3b8;
        word-break: break-all;
    }

    .timestamp {
        color: #475569;
        flex-shrink: 0;
    }

    .level-tag {
        font-weight: 700;
        flex-shrink: 0;
    }

    .level-tag.ai {
        color: #00f0ff;
    }

    .level-tag.info {
        color: #3b82f6;
    }

    .level-tag.warn {
        color: #f59e0b;
    }

    .level-tag.error {
        color: #ef4444;
    }

    .level-tag.success {
        color: #10b981;
    }

    .message-body {
        flex: 1;
        color: #cbd5e1;
    }

    .latency-badge {
        font-size: 0.65rem;
        background: #1e293b;
        color: #a855f7;
        padding: 1px 4px;
        border-radius: 3px;
        flex-shrink: 0;
    }

    .empty-log {
        color: #475569;
        padding: 8px 0;
        font-style: italic;
    }
</style>
