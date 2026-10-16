<script lang="ts">
    interface Props {
        onForceReject?: () => void;
        onInspectTray?: () => void;
        isInspecting?: boolean;
    }

    let { onForceReject, onInspectTray, isInspecting = false }: Props = $props();
</script>

<div class="actions-card">
    <div class="card-header">
        <div class="header-left">
            <span class="card-icon">⚡</span>
            <div>
                <h4>Manual Controls</h4>
                <p class="sub-label">Actuator Triggers</p>
            </div>
        </div>
        <span class="armed-pill">Armed</span>
    </div>

    <div class="buttons-grid">
        <!-- Force Reject Button -->
        <button
            type="button"
            class="action-btn reject"
            onclick={onForceReject}
            title="Actuate pneumatic diverter valve to reject meal tray"
        >
            <div class="btn-icon-circle red">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" class="btn-svg">
                    <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" />
                </svg>
            </div>
            <div class="btn-text">
                <strong>Force Reject</strong>
                <small>Diverter Valve</small>
            </div>
        </button>

        <!-- Inspect Tray Button -->
        <button
            type="button"
            class="action-btn inspect"
            class:loading={isInspecting}
            disabled={isInspecting}
            onclick={onInspectTray}
            title="Trigger single-cycle optical camera acquisition and AI inference"
        >
            <div class="btn-icon-circle green">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" class="btn-svg">
                    <path stroke-linecap="round" stroke-linejoin="round" d="M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z" />
                    <circle cx="12" cy="12" r="9" />
                </svg>
            </div>
            <div class="btn-text">
                <strong>{isInspecting ? 'Running...' : 'Inspect Tray'}</strong>
                <small>Optical Trigger</small>
            </div>
        </button>
    </div>
</div>

<style>
    .actions-card {
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

    .armed-pill {
        font-size: 0.68rem;
        font-weight: 700;
        background: #fef3c7;
        color: #d97706;
        padding: 2px 8px;
        border-radius: var(--radius-pill);
        border: 1px solid #fde68a;
    }

    .buttons-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 10px;
    }

    .action-btn {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 10px 12px;
        background: #f8fafc;
        border: 1px solid #e2e8f0;
        border-radius: var(--radius-md);
        cursor: pointer;
        text-align: left;
        transition: all 0.15s ease;
    }

    .action-btn:hover {
        background: #ffffff;
        border-color: #cbd5e1;
        box-shadow: 0 1px 3px rgba(15, 23, 42, 0.08);
    }

    .action-btn:disabled {
        opacity: 0.5;
        cursor: not-allowed;
    }

    .btn-icon-circle {
        width: 32px;
        height: 32px;
        border-radius: 50%;
        display: flex;
        align-items: center;
        justify-content: center;
        flex-shrink: 0;
    }

    .btn-icon-circle.red {
        background: #fee2e2;
        color: #dc2626;
    }

    .btn-icon-circle.green {
        background: #dcfce7;
        color: #16a34a;
    }

    .btn-svg {
        width: 16px;
        height: 16px;
    }

    .btn-text {
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }

    .btn-text strong {
        font-size: 0.8rem;
        font-weight: 700;
        color: var(--text-primary);
        white-space: nowrap;
    }

    .btn-text small {
        font-size: 0.65rem;
        color: var(--text-muted);
        white-space: nowrap;
    }

    @media (max-width: 400px) {
        .buttons-grid {
            grid-template-columns: 1fr;
        }
    }
</style>
