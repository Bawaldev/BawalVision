<script lang="ts">
    interface Props {
        onForceReject?: () => void;
        onInspectTray?: () => void;
        isInspecting?: boolean;
    }

    let { onForceReject, onInspectTray, isInspecting = false }: Props = $props();
</script>

<div class="action-trigger-wrapper">
    <div class="panel-header">
        <h3>Action & Trigger bar</h3>
        <span class="control-tag">CONVEYOR PLC INTERLOCK</span>
    </div>

    <div class="trigger-buttons-grid">
        <!-- Force Reject Button -->
        <button
            type="button"
            class="trigger-btn reject-btn"
            onclick={onForceReject}
        >
            <div class="btn-icon">⛔</div>
            <div class="btn-text">
                <strong>Force Reject</strong>
                <small>Trigger Pneumatic Ejector</small>
            </div>
        </button>

        <!-- Inspect Tray Button -->
        <button
            type="button"
            class="trigger-btn inspect-btn"
            class:loading={isInspecting}
            disabled={isInspecting}
            onclick={onInspectTray}
        >
            <div class="btn-icon">
                {#if isInspecting}
                    <span class="spin">◌</span>
                {:else}
                    🔍
                {/if}
            </div>
            <div class="btn-text">
                <strong>{isInspecting ? 'Inspecting...' : 'Inspect Tray'}</strong>
                <small>Trigger Optical Camera + AI1</small>
            </div>
        </button>
    </div>
</div>

<style>
    .action-trigger-wrapper {
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

    .control-tag {
        font-size: 0.6rem;
        font-family: monospace;
        color: #64748b;
        background: #080c14;
        padding: 2px 6px;
        border-radius: 4px;
        border: 1px solid #1e293b;
    }

    .trigger-buttons-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 10px;
    }

    .trigger-btn {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 12px;
        border-radius: 6px;
        cursor: pointer;
        text-align: left;
        border: 1px solid transparent;
        transition: all 0.2s ease;
    }

    .trigger-btn:active {
        transform: translateY(1px);
    }

    .reject-btn {
        background: rgba(239, 68, 68, 0.12);
        border-color: rgba(239, 68, 68, 0.35);
        color: #fca5a5;
    }

    .reject-btn:hover {
        background: rgba(239, 68, 68, 0.22);
        border-color: #ef4444;
        color: #ffffff;
        box-shadow: 0 0 12px rgba(239, 68, 68, 0.2);
    }

    .inspect-btn {
        background: rgba(0, 240, 255, 0.12);
        border-color: rgba(0, 240, 255, 0.35);
        color: #a5f3fc;
    }

    .inspect-btn:hover:not(:disabled) {
        background: rgba(0, 240, 255, 0.22);
        border-color: #00f0ff;
        color: #ffffff;
        box-shadow: 0 0 12px rgba(0, 240, 255, 0.2);
    }

    .inspect-btn:disabled {
        opacity: 0.6;
        cursor: not-allowed;
    }

    .btn-icon {
        font-size: 1.3rem;
        line-height: 1;
        flex-shrink: 0;
    }

    .btn-text strong {
        display: block;
        font-size: 0.85rem;
        font-weight: 800;
        line-height: 1.1;
    }

    .btn-text small {
        font-size: 0.6rem;
        color: #94a3b8;
        display: block;
        margin-top: 2px;
    }

    .spin {
        display: inline-block;
        animation: rotate 1s linear infinite;
    }

    @keyframes rotate {
        100% { transform: rotate(360deg); }
    }
</style>
