<script lang="ts">
    import type { SystemConfig } from '../types';

    interface Props {
        config: SystemConfig;
        onSaveConfig: (updated: SystemConfig) => void;
    }

    let { config, onSaveConfig }: Props = $props();

    let formState = $state({
        backendUrl: '/api/v1',
        deviceId: 'ESP32S3_441BF6C95678',
        streamIntervalMs: 1500,
        enableMockIfOffline: true,
        minConfidenceThreshold: 0.35,
        vocWarningThreshold: 80.0,
        vocDangerThreshold: 40.0,
        gravitonEngineEnabled: true,
        localEngineEnabled: true
    });

    $effect(() => {
        if (config) {
            formState.backendUrl = config.backendUrl;
            formState.deviceId = config.deviceId;
            formState.streamIntervalMs = config.streamIntervalMs;
            formState.enableMockIfOffline = config.enableMockIfOffline;
            formState.minConfidenceThreshold = config.minConfidenceThreshold;
            formState.vocWarningThreshold = config.vocWarningThreshold;
            formState.vocDangerThreshold = config.vocDangerThreshold;
            formState.gravitonEngineEnabled = config.gravitonEngineEnabled;
            formState.localEngineEnabled = config.localEngineEnabled;
        }
    });

    let isSaved = $state(false);

    function handleSubmit(e: SubmitEvent) {
        e.preventDefault();
        onSaveConfig(formState);
        isSaved = true;
        setTimeout(() => (isSaved = false), 2500);
    }
</script>

<div class="settings-view-container">
    <div class="card-header-bar">
        <div class="header-left">
            <span class="view-icon">⚙️</span>
            <div>
                <h2>System & Hardware Settings</h2>
                <p class="sub-label">Sensor Thresholds and Device Nodes</p>
            </div>
        </div>
        {#if isSaved}
            <span class="save-toast">✓ Settings Applied</span>
        {/if}
    </div>

    <form onsubmit={handleSubmit} class="settings-form">
        <!-- 1. Gateway & Connectivity -->
        <div class="settings-card">
            <div class="section-header">
                <span class="section-icon">🌐</span>
                <h3>Gateway & Fieldbus Connectivity</h3>
            </div>
            <div class="form-grid">
                <div class="form-group">
                    <label for="backendUrl">Backend API Base URL</label>
                    <input
                        id="backendUrl"
                        type="text"
                        bind:value={formState.backendUrl}
                        class="form-input"
                    />
                    <small>Default: /api/v1 (Proxies to Go Fiber Gateway)</small>
                </div>

                <div class="form-group">
                    <label for="deviceId">ESP32-S3 Sensor Node ID</label>
                    <input
                        id="deviceId"
                        type="text"
                        bind:value={formState.deviceId}
                        class="form-input"
                    />
                    <small>Active device identifier broadcast via telemetry</small>
                </div>
            </div>
        </div>

        <!-- 2. Tolerance & Spoilage Thresholds -->
        <div class="settings-card">
            <div class="section-header">
                <span class="section-icon">🎯</span>
                <h3>Detection & Spoilage Thresholds</h3>
            </div>
            <div class="form-grid">
                <div class="form-group">
                    <label for="minConf">Minimum Confidence Gate ({(formState.minConfidenceThreshold * 100).toFixed(0)}%)</label>
                    <input
                        id="minConf"
                        type="range"
                        min="0.20"
                        max="0.95"
                        step="0.05"
                        bind:value={formState.minConfidenceThreshold}
                        class="form-range"
                    />
                    <small>Vision classification acceptance threshold</small>
                </div>

                <div class="form-group">
                    <label for="vocThreshold">VOC Spoilage Warning Threshold ({formState.vocWarningThreshold} kΩ)</label>
                    <input
                        id="vocThreshold"
                        type="number"
                        bind:value={formState.vocWarningThreshold}
                        class="form-input"
                    />
                    <small>BME680 gas resistance cutoff for spoilage warnings</small>
                </div>
            </div>
        </div>

        <!-- 3. Engine Hardware Accelerators -->
        <div class="settings-card">
            <div class="section-header">
                <span class="section-icon">⚡</span>
                <h3>Hardware Accelerators</h3>
            </div>
            <div class="checkbox-list">
                <label class="check-item" class:disabled-label={!config.gravitonEngineAvailable}>
                    <input
                        type="checkbox"
                        bind:checked={formState.gravitonEngineEnabled}
                        disabled={!config.gravitonEngineAvailable}
                    />
                    <div class="check-text">
                        <strong>
                            AWS Graviton3 (Neoverse-N1 SVE Acceleration)
                            {#if !config.gravitonEngineAvailable}
                                <span class="unavail-badge">Unavailable</span>
                            {/if}
                        </strong>
                        <small>
                            {#if !config.gravitonEngineAvailable}
                                Requires ARM64 Graviton3 cloud hardware instance
                            {:else}
                                Utilizes ARM SVE vector acceleration extensions
                            {/if}
                        </small>
                    </div>
                </label>

                <label class="check-item">
                    <input type="checkbox" bind:checked={formState.localEngineEnabled} />
                    <div class="check-text">
                        <strong>Local C++20 Vision Service (OpenMP SIMD)</strong>
                        <small>Active on-premise zero-dependency machine vision pipeline</small>
                    </div>
                </label>
            </div>
        </div>

        <div class="form-footer">
            <button type="submit" class="save-chip-btn">
                Save & Apply Settings
            </button>
        </div>
    </form>
</div>

<style>
    .settings-view-container {
        display: flex;
        flex-direction: column;
        gap: 16px;
        max-width: 800px;
    }

    .card-header-bar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 12px;
        border-bottom: 1px solid var(--border-subtle);
    }

    .header-left {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .view-icon {
        font-size: 1.5rem;
    }

    .header-left h2 {
        margin: 0;
        font-size: 1.15rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .sub-label {
        margin: 2px 0 0;
        font-size: 0.72rem;
        color: var(--text-muted);
    }

    .save-toast {
        background: var(--accent-green-dim);
        color: var(--accent-green);
        border: 1px solid rgba(34, 197, 94, 0.3);
        padding: 4px 12px;
        border-radius: var(--radius-pill);
        font-size: 0.75rem;
        font-weight: 700;
    }

    .settings-form {
        display: flex;
        flex-direction: column;
        gap: 16px;
    }

    .settings-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        padding: 18px;
        display: flex;
        flex-direction: column;
        gap: 14px;
        box-shadow: var(--shadow-card);
    }

    .section-header {
        display: flex;
        align-items: center;
        gap: 8px;
        border-bottom: 1px solid var(--border-subtle);
        padding-bottom: 8px;
    }

    .section-icon {
        font-size: 1.1rem;
    }

    .section-header h3 {
        margin: 0;
        font-size: 0.92rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .form-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 14px;
    }

    .form-group {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .form-group label {
        font-size: 0.75rem;
        font-weight: 600;
        color: var(--text-secondary);
    }

    .form-input {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        color: var(--text-primary);
        font-family: var(--font-sans);
        font-size: 0.82rem;
        padding: 8px 12px;
        border-radius: var(--radius-md);
        outline: none;
    }

    .form-input:focus {
        border-color: var(--accent-blue);
    }

    .form-range {
        accent-color: var(--accent-blue);
        cursor: pointer;
    }

    .form-group small {
        font-size: 0.68rem;
        color: var(--text-muted);
    }

    .checkbox-list {
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .check-item {
        display: flex;
        align-items: center;
        gap: 12px;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        padding: 12px 14px;
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .check-item:hover:not(.disabled-label) {
        background: var(--bg-card-hover);
        border-color: var(--border-highlight);
    }

    .check-item.disabled-label {
        opacity: 0.5;
        cursor: not-allowed;
    }

    .check-item input {
        width: 18px;
        height: 18px;
        accent-color: var(--accent-blue);
        cursor: pointer;
    }

    .check-item.disabled-label input {
        cursor: not-allowed;
    }

    .unavail-badge {
        display: inline-block;
        font-size: 0.65rem;
        font-weight: 700;
        color: var(--accent-red);
        background: var(--accent-red-dim);
        border: 1px solid rgba(244, 63, 94, 0.3);
        padding: 1px 6px;
        border-radius: var(--radius-pill);
        margin-left: 6px;
    }

    .check-text strong {
        display: block;
        font-size: 0.82rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .check-text small {
        font-size: 0.7rem;
        color: var(--text-muted);
    }

    .form-footer {
        padding-top: 8px;
    }

    .save-chip-btn {
        background: var(--accent-blue);
        color: #0f172a;
        font-weight: 700;
        font-size: 0.85rem;
        padding: 10px 24px;
        border-radius: var(--radius-pill);
        border: none;
        cursor: pointer;
        transition: opacity 0.15s ease;
    }

    .save-chip-btn:hover {
        opacity: 0.9;
    }
</style>
