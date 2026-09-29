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
    <div class="view-header">
        <div>
            <h2>System & Cloud Settings</h2>
            <p class="muted">Configure Edge Microcontroller Links, Vision Model Parameters, and AWS Cloud Endpoints</p>
        </div>
        {#if isSaved}
            <span class="save-toast">✓ Settings Saved Successfully</span>
        {/if}
    </div>

    <form onsubmit={handleSubmit} class="settings-form">
        <!-- 1. Gateway & Connectivity -->
        <div class="settings-section">
            <h3>1. Backend Gateway & Hardware Links</h3>
            <div class="form-grid">
                <div class="form-group">
                    <label for="backendUrl">Backend API Base URL</label>
                    <input
                        id="backendUrl"
                        type="text"
                        bind:value={formState.backendUrl}
                        class="form-input"
                    />
                    <small>Default: /api/v1 (proxies to Go Fiber Gateway :8080)</small>
                </div>

                <div class="form-group">
                    <label for="deviceId">ESP32-S3 Camera Node ID</label>
                    <input
                        id="deviceId"
                        type="text"
                        bind:value={formState.deviceId}
                        class="form-input"
                    />
                    <small>Active serial/MAC identifier broadcast by edge unit</small>
                </div>
            </div>
        </div>

        <!-- 2. AI Inference & Spoilage Thresholds -->
        <div class="settings-section">
            <h3>2. AI Vision & Spoilage Thresholds</h3>
            <div class="form-grid">
                <div class="form-group">
                    <label for="minConf">Minimum Detection Confidence ({(formState.minConfidenceThreshold * 100).toFixed(0)}%)</label>
                    <input
                        id="minConf"
                        type="range"
                        min="0.20"
                        max="0.95"
                        step="0.05"
                        bind:value={formState.minConfidenceThreshold}
                        class="form-range"
                    />
                    <small>Compartment classification cutoff for MobileNetV3 ONNX</small>
                </div>

                <div class="form-group">
                    <label for="vocThreshold">VOC Spoilage Alert Level ({formState.vocWarningThreshold} kΩ)</label>
                    <input
                        id="vocThreshold"
                        type="number"
                        bind:value={formState.vocWarningThreshold}
                        class="form-input"
                    />
                    <small>Gas resistance below this value triggers spoilage warning</small>
                </div>
            </div>
        </div>

        <!-- 3. Engine Hardware Accelerators -->
        <div class="settings-section">
            <h3>3. Compute Engine Acceleration</h3>
            <div class="checkbox-list">
                <label class="check-label">
                    <input type="checkbox" bind:checked={formState.gravitonEngineEnabled} />
                    <div>
                        <strong>AWS Graviton3 (Neoverse-N1 SVE Acceleration)</strong>
                        <small>Utilizes SIMD vector extensions on AWS Cloud instances</small>
                    </div>
                </label>

                <label class="check-label">
                    <input type="checkbox" bind:checked={formState.localEngineEnabled} />
                    <div>
                        <strong>Local Engine (OpenMP SIMD C++20 Vision Service)</strong>
                        <small>Enables on-premise zero-dependency inference pipeline</small>
                    </div>
                </label>
            </div>
        </div>

        <div class="form-footer">
            <button type="submit" class="save-btn">
                Save & Apply Configuration
            </button>
        </div>
    </form>
</div>

<style>
    .settings-view-container {
        display: flex;
        flex-direction: column;
        gap: 16px;
        max-width: 900px;
    }

    .view-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 12px;
        border-bottom: 1px solid var(--border-color, #1f293d);
    }

    .view-header h2 {
        margin: 0;
        font-size: 1.3rem;
        color: #ffffff;
    }

    .muted {
        margin: 4px 0 0;
        font-size: 0.8rem;
        color: #64748b;
    }

    .save-toast {
        background: rgba(16, 185, 129, 0.2);
        color: #10b981;
        border: 1px solid rgba(16, 185, 129, 0.4);
        padding: 6px 12px;
        border-radius: 4px;
        font-size: 0.75rem;
        font-weight: 700;
    }

    .settings-form {
        display: flex;
        flex-direction: column;
        gap: 16px;
    }

    .settings-section {
        background: var(--bg-panel, #121824);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
        padding: 16px;
        display: flex;
        flex-direction: column;
        gap: 12px;
    }

    .settings-section h3 {
        margin: 0;
        font-size: 0.95rem;
        color: #00f0ff;
        border-bottom: 1px solid #1e293b;
        padding-bottom: 8px;
    }

    .form-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 16px;
    }

    .form-group {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .form-group label {
        font-size: 0.75rem;
        font-weight: 700;
        color: #cbd5e1;
    }

    .form-input {
        background: #0b0f19;
        border: 1px solid #1e293b;
        color: #ffffff;
        font-size: 0.85rem;
        padding: 8px 12px;
        border-radius: 6px;
    }

    .form-range {
        accent-color: #00f0ff;
        cursor: pointer;
    }

    .form-group small {
        font-size: 0.65rem;
        color: #64748b;
    }

    .checkbox-list {
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .check-label {
        display: flex;
        align-items: center;
        gap: 12px;
        background: #0b0f19;
        border: 1px solid #1e293b;
        border-radius: 6px;
        padding: 12px;
        cursor: pointer;
    }

    .check-label input {
        width: 18px;
        height: 18px;
        accent-color: #00f0ff;
        cursor: pointer;
    }

    .check-label strong {
        display: block;
        font-size: 0.85rem;
        color: #f1f5f9;
    }

    .check-label small {
        font-size: 0.65rem;
        color: #64748b;
    }

    .form-footer {
        padding-top: 8px;
    }

    .save-btn {
        background: linear-gradient(90deg, #00f0ff, #3b82f6);
        color: #000;
        font-weight: 800;
        font-size: 0.85rem;
        padding: 10px 24px;
        border: none;
        border-radius: 6px;
        cursor: pointer;
        transition: all 0.2s ease;
    }

    .save-btn:hover {
        opacity: 0.9;
        box-shadow: 0 0 16px rgba(0, 240, 255, 0.4);
    }
</style>
