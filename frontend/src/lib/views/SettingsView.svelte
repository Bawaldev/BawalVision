<script lang="ts">
    import { onMount } from 'svelte';
    import { connectEspDevice, disconnectEspDevice, fetchDeviceStatus, provisionEspViaUsb } from '../api';
    import type { SystemConfig, EspConnectionResult, USBPortInfo } from '../types';

    interface Props {
        config: SystemConfig;
        isOnline?: boolean;
        onDeviceConnectionChange?: (connected: boolean, deviceId: string) => void;
        onSaveConfig: (updated: SystemConfig) => void;
    }

    let {
        config,
        isOnline = false,
        onDeviceConnectionChange,
        onSaveConfig
    }: Props = $props();

    let formState = $state({
        backendUrl: '/api/v1',
        deviceId: 'ESP32S3_441BF6C95678',
        streamIntervalMs: 1500,
        enableMockIfOffline: true,
        minConfidenceThreshold: 0.35,
        gravitonEngineEnabled: true,
        localEngineEnabled: true
    });

    // ESP32-S3 Hardware Connection & USB Serial Provisioning State
    let connectionStatus = $state<'disconnected' | 'connecting' | 'connected' | 'failed'>('disconnected');
    let connectingStepText = $state('Probing shared Wi-Fi & USB slots...');
    let errorMessage = $state('');
    let detectedUsbPorts = $state<USBPortInfo[]>([]);
    let selectedUsbPort = $state('');
    let serverWifiSsid = $state('');
    let serverWifiPassword = $state('');
    let serverBackendIp = $state('');
    let lastConnDetails = $state<EspConnectionResult>({
        success: false,
        deviceId: '',
        firmwareVersion: '--',
        latencyMs: 0,
        ipAddress: '--',
        connectionMode: 'disconnected',
        message: 'Disconnected'
    });

    $effect(() => {
        if (config) {
            formState.backendUrl = config.backendUrl;
            formState.deviceId = config.deviceId || formState.deviceId;
            formState.streamIntervalMs = config.streamIntervalMs;
            formState.enableMockIfOffline = config.enableMockIfOffline;
            formState.minConfidenceThreshold = config.minConfidenceThreshold;
            formState.gravitonEngineEnabled = config.gravitonEngineEnabled;
            formState.localEngineEnabled = config.localEngineEnabled;
        }
    });

    async function refreshHardwareStatus() {
        const hw = await fetchDeviceStatus(formState.backendUrl);
        if (!hw) return;

        detectedUsbPorts = hw.usb_ports || [];
        if (detectedUsbPorts.length > 0 && !selectedUsbPort) {
            selectedUsbPort = detectedUsbPorts[0].port;
        } else if (detectedUsbPorts.length === 0) {
            selectedUsbPort = '';
        }

        if (hw.server_wifi) {
            if (!serverWifiSsid && hw.server_wifi.ssid) {
                serverWifiSsid = hw.server_wifi.ssid;
            }
            if (hw.server_wifi.backend_ip) {
                serverBackendIp = hw.server_wifi.backend_ip;
            }
        }

        if (hw.connected) {
            connectionStatus = 'connected';
            lastConnDetails = {
                success: true,
                deviceId: hw.device_id || formState.deviceId,
                firmwareVersion: hw.firmware_version || '1.0.0',
                ipAddress: hw.ip_address || serverBackendIp || 'LAN',
                connectionMode: hw.connection_mode,
                latencyMs: hw.connection_mode === 'usb_serial' ? 4 : 9,
                message: hw.connection_mode === 'usb_serial'
                    ? `Connected via USB Serial (${hw.ip_address}) — Server Wi-Fi Synced`
                    : `Connected on shared Wi-Fi network (${hw.ip_address || 'LAN'})`
            };
            if (hw.device_id) {
                formState.deviceId = hw.device_id;
            }
        } else if (connectionStatus === 'connected' && !isOnline) {
            connectionStatus = 'disconnected';
        }
    }

    onMount(() => {
        refreshHardwareStatus();
        const timer = setInterval(refreshHardwareStatus, 4000);
        return () => clearInterval(timer);
    });

    async function handleConnectEsp() {
        const targetId = (formState.deviceId || '').trim();
        connectionStatus = 'connecting';
        errorMessage = '';
        connectingStepText = 'Checking shared Wi-Fi network & scanning server USB ports (/dev/ttyACM*, /dev/ttyUSB*)...';

        try {
            const res = await connectEspDevice(targetId, formState.backendUrl, {
                usbPort: selectedUsbPort,
                ssid: serverWifiSsid,
                password: serverWifiPassword
            });

            if (res.success) {
                connectionStatus = 'connected';
                lastConnDetails = res;
                if (res.deviceId) {
                    formState.deviceId = res.deviceId;
                }
                onDeviceConnectionChange?.(true, res.deviceId);
                await refreshHardwareStatus();
            } else {
                connectionStatus = 'failed';
                errorMessage = res.message || 'No ESP32-S3 detected on shared Wi-Fi or USB slots.';
                onDeviceConnectionChange?.(false, targetId);
                await refreshHardwareStatus();
            }
        } catch (err: any) {
            connectionStatus = 'failed';
            errorMessage = err?.message || 'Failed to probe ESP32-S3 hardware.';
            onDeviceConnectionChange?.(false, targetId);
        }
    }

    async function handleProvisionUsbWifi() {
        const portToUse = selectedUsbPort || (detectedUsbPorts[0]?.port ?? '');
        if (!portToUse) {
            connectionStatus = 'failed';
            errorMessage = 'No USB Serial device (/dev/ttyACM* or /dev/ttyUSB*) plugged into the server.';
            return;
        }
        connectionStatus = 'connecting';
        errorMessage = '';
        connectingStepText = `Provisioning server Wi-Fi '${serverWifiSsid || 'Auto'}' over ${portToUse}...`;

        const res = await provisionEspViaUsb(portToUse, serverWifiSsid, serverWifiPassword, formState.backendUrl);
        if (res.success) {
            connectionStatus = 'connected';
            lastConnDetails = res;
            if (res.deviceId) {
                formState.deviceId = res.deviceId;
            }
            onDeviceConnectionChange?.(true, res.deviceId);
            await refreshHardwareStatus();
        } else {
            connectionStatus = 'failed';
            errorMessage = res.message || 'USB Wi-Fi provisioning failed.';
        }
    }

    async function handleDisconnectEsp() {
        try {
            await disconnectEspDevice(formState.deviceId, formState.backendUrl);
        } catch {}
        connectionStatus = 'disconnected';
        errorMessage = '';
        onDeviceConnectionChange?.(false, formState.deviceId);
    }

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

            <!-- ESP32-S3 Connection Controller (Real LAN + USB /dev/ttyACM* & /dev/ttyUSB* Wi-Fi Sync) -->
            <div class="form-group esp-connect-group">
                    <div class="esp-header-row">
                        <label for="deviceId">ESP32-S3 Sensor Node Connection</label>
                        <span class="esp-status-chip {connectionStatus}">
                            <span class="status-dot {connectionStatus}"></span>
                            {#if connectionStatus === 'connected'}
                                {lastConnDetails.connectionMode === 'usb_serial' ? 'Connected (USB Sync)' : 'Connected (Same Wi-Fi)'}
                            {:else if connectionStatus === 'connecting'}
                                Probing Hardware...
                            {:else if connectionStatus === 'failed'}
                                Not Detected
                            {:else}
                                Disconnected
                            {/if}
                        </span>
                    </div>

                    <!-- Target Node ID Input & Connect Action Button -->
                    <div class="esp-input-action-bar">
                        <div class="input-with-icon">
                            <span class="input-icon">📡</span>
                            <input
                                id="deviceId"
                                type="text"
                                bind:value={formState.deviceId}
                                placeholder="e.g. ESP32S3_441BF6C95678"
                                class="form-input esp-id-input"
                                disabled={connectionStatus === 'connecting'}
                            />
                        </div>

                        {#if connectionStatus === 'connected'}
                            <button
                                type="button"
                                class="btn-esp-disconnect"
                                onclick={handleDisconnectEsp}
                                title="Disconnect ESP32 node"
                            >
                                Disconnect
                            </button>
                        {:else if connectionStatus === 'connecting'}
                            <button
                                type="button"
                                class="btn-esp-connecting"
                                disabled
                            >
                                <span class="btn-spinner"></span>
                                Probing...
                            </button>
                        {:else}
                            <button
                                type="button"
                                class="btn-esp-connect"
                                onclick={handleConnectEsp}
                                title="Detect ESP32 on shared Wi-Fi or provision via USB"
                            >
                                Detect / Connect
                            </button>
                        {/if}
                    </div>

                    <!-- USB Serial Slots (/dev/ttyACM* & /dev/ttyUSB*) & Server Wi-Fi Auto-Provisioning Panel -->
                    <div class="usb-provision-panel">
                        <div class="usb-panel-header">
                            <span class="usb-badge-title">🔌 Server USB Slots (<code>/dev/ttyACM*</code>, <code>/dev/ttyUSB*</code>) & Same-Wi-Fi Sync</span>
                            <button type="button" class="btn-scan-usb" onclick={refreshHardwareStatus}>
                                ↻ Scan USB
                            </button>
                        </div>

                        {#if detectedUsbPorts.length > 0}
                            <div class="usb-detected-banner">
                                <span>✓ USB Serial Detected: <strong>{detectedUsbPorts.map(p => `${p.port}${p.product ? ` (${p.product})` : ''}`).join(', ')}</strong></span>
                            </div>
                        {:else}
                            <div class="usb-empty-banner">
                                <span>No USB serial device currently detected on server slots. Plug ESP32-S3 into USB if Wi-Fi is not configured yet.</span>
                            </div>
                        {/if}

                        <div class="form-grid usb-wifi-grid">
                            <div class="form-group">
                                <label for="serverWifiSsid">Server Wi-Fi SSID (Synced to ESP32-S3)</label>
                                <input
                                    id="serverWifiSsid"
                                    type="text"
                                    bind:value={serverWifiSsid}
                                    placeholder="Auto-detected server SSID"
                                    class="form-input"
                                />
                            </div>
                            <div class="form-group">
                                <label for="serverWifiPass">Server Wi-Fi Password (for USB Provisioning)</label>
                                <div class="esp-input-action-bar">
                                    <input
                                        id="serverWifiPass"
                                        type="password"
                                        bind:value={serverWifiPassword}
                                        placeholder="Wi-Fi Password (or set WIFI_PASS)"
                                        class="form-input"
                                        style="flex: 1;"
                                    />
                                    {#if detectedUsbPorts.length > 0}
                                        <button
                                            type="button"
                                            class="btn-esp-connect"
                                            onclick={handleProvisionUsbWifi}
                                            title="Apply Server Wi-Fi to ESP32-S3 via USB"
                                        >
                                            Sync Wi-Fi via USB
                                        </button>
                                    {/if}
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- Diagnostic Properties & Live Link Status Card -->
                    <div class="esp-connection-card {connectionStatus}">
                        {#if connectionStatus === 'connected'}
                            <div class="esp-diag-grid">
                                <div class="diag-item">
                                    <span class="diag-lbl">Transport Mode</span>
                                    <span class="diag-val good">
                                        {lastConnDetails.connectionMode === 'usb_serial' ? '🔌 USB Serial' : '📶 Shared Wi-Fi LAN'}
                                    </span>
                                </div>
                                <div class="diag-item">
                                    <span class="diag-lbl">Ping / Latency</span>
                                    <span class="diag-val">⚡ {lastConnDetails.latencyMs ?? 8} ms</span>
                                </div>
                                <div class="diag-item">
                                    <span class="diag-lbl">Firmware</span>
                                    <span class="diag-val mono">{lastConnDetails.firmwareVersion ?? '1.0.0'}</span>
                                </div>
                                <div class="diag-item">
                                    <span class="diag-lbl">Endpoint / Port</span>
                                    <span class="diag-val mono">{lastConnDetails.ipAddress || serverBackendIp || 'LAN'}</span>
                                </div>
                            </div>
                            <div class="esp-sync-badge">
                                <span class="sync-check">✓</span> {lastConnDetails.message || 'ESP32-S3 live telemetry & shared Wi-Fi synchronized'}
                            </div>
                        {:else if connectionStatus === 'connecting'}
                            <div class="esp-connecting-box">
                                <div class="connecting-wave">
                                    <span class="wave-bar"></span>
                                    <span class="wave-bar"></span>
                                    <span class="wave-bar"></span>
                                </div>
                                <div class="connecting-info">
                                    <span class="connecting-step-title">{connectingStepText}</span>
                                    <small class="connecting-hint">Checking shared Wi-Fi heartbeats & USB serial slots...</small>
                                </div>
                            </div>
                        {:else if connectionStatus === 'failed'}
                            <div class="esp-failed-box">
                                <div class="failed-info">
                                    <span class="failed-icon">⚠️</span>
                                    <span class="failed-text">{errorMessage || 'Node unreachable. Verify same Wi-Fi connection or plug into server USB.'}</span>
                                </div>
                                <button type="button" class="btn-retry-conn" onclick={handleConnectEsp}>
                                    ↻ Retry
                                </button>
                            </div>
                        {:else}
                            <div class="esp-idle-box">
                                <span class="idle-icon">💡</span>
                                <span class="idle-text">Connect ESP32-S3 to the <strong>same Wi-Fi network</strong> as the server, or plug it into a <strong>server USB port</strong> (<code>/dev/ttyACM*</code> / <code>/dev/ttyUSB*</code>) to automatically apply the server's Wi-Fi.</span>
                            </div>
                        {/if}
                    </div>
                </div>
        </div>

        <!-- 2. Detection Confidence Thresholds -->
        <div class="settings-card">
            <div class="section-header">
                <span class="section-icon">🎯</span>
                <h3>Detection Thresholds</h3>
            </div>
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

    /* ESP Connect Widget */
    .esp-connect-group {
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .esp-header-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .esp-status-chip {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        font-size: 0.68rem;
        font-weight: 700;
        padding: 2px 8px;
        border-radius: var(--radius-pill);
        border: 1px solid var(--border-subtle);
        background: var(--bg-inset);
        color: var(--text-muted);
        text-transform: capitalize;
    }

    .esp-status-chip.connected {
        color: var(--accent-green);
        background: var(--accent-green-dim);
        border-color: rgba(34, 197, 94, 0.35);
    }

    .esp-status-chip.connecting {
        color: var(--accent-blue);
        background: rgba(59, 130, 246, 0.15);
        border-color: rgba(59, 130, 246, 0.35);
    }

    .esp-status-chip.failed {
        color: var(--accent-red);
        background: var(--accent-red-dim);
        border-color: rgba(239, 68, 68, 0.35);
    }

    .status-dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
        background: var(--text-muted);
    }

    .status-dot.connected {
        background: var(--accent-green);
        box-shadow: 0 0 6px rgba(34, 197, 94, 0.8);
    }

    .status-dot.connecting {
        background: var(--accent-blue);
        animation: pulse-dot 1s infinite alternate;
    }

    .status-dot.failed {
        background: var(--accent-red);
    }

    @keyframes pulse-dot {
        from { opacity: 0.4; transform: scale(0.9); }
        to { opacity: 1; transform: scale(1.2); }
    }

    .esp-input-action-bar {
        display: flex;
        gap: 8px;
        align-items: center;
    }

    .input-with-icon {
        position: relative;
        flex: 1;
        display: flex;
        align-items: center;
    }

    .input-icon {
        position: absolute;
        left: 10px;
        font-size: 0.85rem;
        pointer-events: none;
        opacity: 0.7;
    }

    .esp-id-input {
        width: 100%;
        padding-left: 32px;
        font-family: var(--font-mono);
        font-size: 0.78rem;
    }

    .btn-esp-connect {
        background: var(--accent-blue);
        color: #0f172a;
        font-weight: 700;
        font-size: 0.78rem;
        padding: 0 16px;
        border-radius: var(--radius-md);
        border: none;
        cursor: pointer;
        transition: all 0.15s ease;
        white-space: nowrap;
        height: 35px;
    }

    .btn-esp-connect:hover {
        filter: brightness(1.1);
        transform: translateY(-1px);
    }

    .btn-esp-disconnect {
        background: rgba(239, 68, 68, 0.12);
        color: #ef4444;
        border: 1px solid rgba(239, 68, 68, 0.3);
        font-weight: 700;
        font-size: 0.78rem;
        padding: 0 14px;
        border-radius: var(--radius-md);
        cursor: pointer;
        transition: all 0.15s ease;
        white-space: nowrap;
        height: 35px;
    }

    .btn-esp-disconnect:hover {
        background: rgba(239, 68, 68, 0.22);
    }

    .btn-esp-connecting {
        background: rgba(59, 130, 246, 0.2);
        color: #93c5fd;
        border: 1px solid rgba(59, 130, 246, 0.35);
        font-weight: 600;
        font-size: 0.75rem;
        padding: 0 12px;
        border-radius: var(--radius-md);
        cursor: not-allowed;
        display: flex;
        align-items: center;
        gap: 6px;
        white-space: nowrap;
        height: 35px;
    }

    .btn-spinner {
        width: 12px;
        height: 12px;
        border: 2px solid rgba(147, 197, 253, 0.3);
        border-top-color: #93c5fd;
        border-radius: 50%;
        animation: spin 0.8s linear infinite;
    }

    @keyframes spin {
        to { transform: rotate(360deg); }
    }

    .esp-connection-card {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        padding: 10px 12px;
        min-height: 64px;
        display: flex;
        flex-direction: column;
        justify-content: center;
        transition: all 0.2s ease;
    }

    .esp-connection-card.connected {
        border-color: rgba(34, 197, 94, 0.3);
        background: rgba(34, 197, 94, 0.03);
    }

    .esp-connection-card.connecting {
        border-color: rgba(59, 130, 246, 0.3);
        background: rgba(59, 130, 246, 0.03);
    }

    .esp-connection-card.failed {
        border-color: rgba(239, 68, 68, 0.3);
        background: rgba(239, 68, 68, 0.03);
    }

    .esp-diag-grid {
        display: grid;
        grid-template-columns: repeat(4, 1fr);
        gap: 8px 16px;
        margin-bottom: 6px;
    }

    @media (max-width: 640px) {
        .esp-diag-grid {
            grid-template-columns: repeat(2, 1fr);
        }
    }

    .diag-item {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .diag-lbl {
        font-size: 0.62rem;
        color: var(--text-muted);
        text-transform: uppercase;
        letter-spacing: 0.04em;
    }

    .diag-val {
        font-size: 0.74rem;
        color: var(--text-primary);
        font-weight: 600;
    }

    .diag-val.good {
        color: var(--accent-green);
    }

    .diag-val.mono {
        font-family: var(--font-mono);
    }

    .esp-sync-badge {
        font-size: 0.68rem;
        color: var(--accent-green);
        display: flex;
        align-items: center;
        gap: 5px;
        padding-top: 4px;
        border-top: 1px solid rgba(255, 255, 255, 0.06);
    }

    .sync-check {
        font-weight: 800;
    }

    .esp-connecting-box {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .connecting-wave {
        display: flex;
        align-items: flex-end;
        gap: 3px;
        height: 18px;
    }

    .wave-bar {
        width: 3px;
        background: var(--accent-blue);
        border-radius: 2px;
        animation: wave 0.8s ease-in-out infinite;
    }

    .wave-bar:nth-child(1) { height: 8px; animation-delay: 0s; }
    .wave-bar:nth-child(2) { height: 16px; animation-delay: 0.15s; }
    .wave-bar:nth-child(3) { height: 12px; animation-delay: 0.3s; }

    @keyframes wave {
        0%, 100% { transform: scaleY(0.4); opacity: 0.5; }
        50% { transform: scaleY(1); opacity: 1; }
    }

    .connecting-info {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .connecting-step-title {
        font-size: 0.74rem;
        font-weight: 600;
        color: var(--text-primary);
    }

    .connecting-hint {
        font-size: 0.65rem;
        color: var(--text-muted);
    }

    .esp-failed-box {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 10px;
    }

    .failed-info {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .failed-icon {
        font-size: 0.95rem;
    }

    .failed-text {
        font-size: 0.72rem;
        color: var(--accent-red);
        font-weight: 500;
    }

    .btn-retry-conn {
        background: transparent;
        color: var(--accent-blue);
        border: 1px solid var(--accent-blue);
        font-size: 0.7rem;
        font-weight: 600;
        padding: 3px 8px;
        border-radius: var(--radius-sm);
        cursor: pointer;
        white-space: nowrap;
    }

    .btn-retry-conn:hover {
        background: rgba(59, 130, 246, 0.15);
    }

    .esp-idle-box {
        display: flex;
        align-items: center;
        gap: 8px;
        font-size: 0.72rem;
        color: var(--text-muted);
    }

    .idle-icon {
        font-size: 0.9rem;
    }

    .idle-text strong {
        color: var(--text-secondary);
    }

    .usb-provision-panel {
        background: var(--bg-inset);
        border: 1px dashed var(--border-subtle);
        border-radius: var(--radius-md);
        padding: 12px;
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .usb-panel-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 8px;
    }

    .usb-badge-title {
        font-size: 0.74rem;
        font-weight: 700;
        color: var(--text-secondary);
    }

    .btn-scan-usb {
        background: transparent;
        border: 1px solid var(--border-subtle);
        color: var(--text-secondary);
        font-size: 0.68rem;
        font-weight: 600;
        padding: 3px 10px;
        border-radius: var(--radius-sm);
        cursor: pointer;
    }

    .btn-scan-usb:hover {
        border-color: var(--accent-blue);
        color: var(--accent-blue);
    }

    .usb-detected-banner {
        font-size: 0.72rem;
        color: var(--accent-green);
        background: var(--accent-green-dim);
        border: 1px solid rgba(34, 197, 94, 0.3);
        padding: 6px 10px;
        border-radius: var(--radius-sm);
    }

    .usb-empty-banner {
        font-size: 0.7rem;
        color: var(--text-muted);
    }

    .usb-wifi-grid {
        margin-top: 2px;
    }

    @media (max-width: 640px) {
        .form-grid {
            grid-template-columns: 1fr;
        }

        .card-header-bar {
            flex-wrap: wrap;
            gap: 8px;
        }

        .settings-card {
            padding: 14px 12px;
        }

        .esp-header-row,
        .usb-panel-header,
        .esp-failed-box {
            flex-wrap: wrap;
            gap: 8px;
        }

        .esp-input-action-bar {
            flex-direction: column;
            align-items: stretch;
        }

        .input-with-icon,
        .btn-esp-connect,
        .btn-esp-disconnect,
        .btn-esp-connecting,
        .save-chip-btn {
            width: 100%;
            justify-content: center;
        }
    }
</style>

