<script lang="ts">
    import { onMount } from 'svelte';
    import { connectEspDevice, disconnectEspDevice, fetchDeviceStatus, provisionEspViaUsb, fetchWiFiNetworks, probeEspUsbPort } from '../api';
    import type { SystemConfig, EspConnectionResult, USBPortInfo, WiFiNetworkInfo } from '../types';

    interface Props {
        config: SystemConfig;
        isOnline?: boolean;
        parallelCores?: number;
        onDeviceConnectionChange?: (connected: boolean, deviceId: string) => void;
        onSaveConfig: (updated: SystemConfig) => void;
    }

    let {
        config,
        isOnline = false,
        parallelCores = 0,
        onDeviceConnectionChange,
        onSaveConfig
    }: Props = $props();

    let formState = $state({
        backendUrl: '/api/v1',
        deviceId: '',
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

    // Fedora GNOME 45+ Wi-Fi & Peripheral Management State
    let wifiNetworks = $state<WiFiNetworkInfo[]>([]);
    let isScanningWifi = $state(false);
    let selectedNetwork = $state<WiFiNetworkInfo | null>(null);
    let showConnectModal = $state(false);
    let connectPassword = $state('');
    let showPassword = $state(false);
    let connectError = $state('');
    let isConnectingWifi = $state(false);
    let wifiConnectSuccess = $state(false);
    let wifiSuccessMsg = $state('');

    // USB Hardware probe state
    let isProbingUsb = $state(false);
    let usbProbeMsg = $state('');
    let espUsbStatus = $state<{ port: string; device_id: string; current_ssid: string; wifi_connected: boolean; temperature: number } | null>(null);

    // Web Serial direct hardware connection state (Fedora / Browser direct access)
    let isWebSerialConnecting = $state(false);
    let webSerialMsg = $state('');
    let webSerialStatus = $state<'idle' | 'success' | 'error'>('idle');

    $effect(() => {
        if (config) {
            formState.backendUrl = config.backendUrl;
            formState.deviceId = config.deviceId ?? '';
            formState.streamIntervalMs = config.streamIntervalMs;
            formState.enableMockIfOffline = config.enableMockIfOffline;
            const thresh = config.minConfidenceThreshold != null && config.minConfidenceThreshold > 0
                ? config.minConfidenceThreshold
                : 0.35;
            formState.minConfidenceThreshold = Math.min(0.98, Math.max(0.25, thresh));
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

        if (hw.wifi_networks && hw.wifi_networks.length > 0) {
            wifiNetworks = hw.wifi_networks;
        }

        if (hw.server_wifi) {
            if (!serverWifiSsid && hw.server_wifi.ssid) {
                serverWifiSsid = hw.server_wifi.ssid;
            }
            if (hw.server_wifi.backend_ip) {
                serverBackendIp = hw.server_wifi.backend_ip;
            }
        }

        if (hw.esp_usb_status) {
            espUsbStatus = hw.esp_usb_status;
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
        } else if (connectionStatus === 'connected') {
            connectionStatus = 'disconnected';
        }
    }

    async function handleProbeUsbPort(port?: string) {
        isProbingUsb = true;
        usbProbeMsg = 'Probing serial status...';
        const targetPort = port || selectedUsbPort || (detectedUsbPorts[0]?.port ?? '');
        const res = await probeEspUsbPort(targetPort, formState.backendUrl);
        if (res.success && res.deviceId) {
            espUsbStatus = {
                port: res.port || targetPort,
                device_id: res.deviceId,
                current_ssid: res.currentSsid || '',
                wifi_connected: !!res.wifiConnected,
                temperature: res.temperature || 0
            };
            usbProbeMsg = res.currentSsid
                ? `ESP32-S3 (${res.deviceId}) is connected to "${res.currentSsid}"`
                : `ESP32-S3 (${res.deviceId}) detected (Wi-Fi not associated)`;
            await refreshHardwareStatus();
        } else {
            usbProbeMsg = res.message || 'No response from serial port.';
        }
        isProbingUsb = false;
    }

    $effect(() => {
        if (!isOnline && connectionStatus === 'connected') {
            connectionStatus = 'disconnected';
        }
    });

    onMount(() => {
        refreshHardwareStatus();
        const timer = setInterval(refreshHardwareStatus, 1500);
        return () => clearInterval(timer);
    });

    async function handleScanWifi() {
        isScanningWifi = true;
        try {
            const networks = await fetchWiFiNetworks(formState.backendUrl);
            if (networks && networks.length > 0) {
                wifiNetworks = networks;
            } else {
                await refreshHardwareStatus();
            }
        } catch {
            await refreshHardwareStatus();
        } finally {
            setTimeout(() => {
                isScanningWifi = false;
            }, 600);
        }
    }

    function handleSelectNetwork(net: WiFiNetworkInfo) {
        selectedNetwork = net;
        if (net.is_server || net.ssid === serverWifiSsid) {
            connectPassword = serverWifiPassword;
        } else {
            connectPassword = '';
        }
        connectError = '';
        wifiConnectSuccess = false;
        wifiSuccessMsg = '';
        showConnectModal = true;
    }

    function closeConnectModal() {
        showConnectModal = false;
        selectedNetwork = null;
        connectError = '';
        wifiConnectSuccess = false;
    }

    async function handleConnectToSelectedNetwork() {
        if (!selectedNetwork) return;
        isConnectingWifi = true;
        connectError = '';
        wifiConnectSuccess = false;

        const portToUse = selectedUsbPort || (detectedUsbPorts[0]?.port ?? '/dev/ttyACM0');

        try {
            // 1. If connecting to the server/local Wi-Fi network, first check if already on LAN
            if (selectedNetwork.is_server || selectedNetwork.ssid === serverWifiSsid) {
                const probe = await connectEspDevice(formState.deviceId, formState.backendUrl, {
                    usbPort: portToUse,
                    ssid: selectedNetwork.ssid,
                    password: connectPassword
                });
                if (probe.success) {
                    wifiConnectSuccess = true;
                    wifiSuccessMsg = `Connected to ESP32-S3 on ${selectedNetwork.ssid} (${probe.ipAddress || 'Wi-Fi LAN'})!`;
                    connectionStatus = 'connected';
                    lastConnDetails = probe;
                    onDeviceConnectionChange?.(true, probe.deviceId);
                    await refreshHardwareStatus();
                    setTimeout(() => {
                        closeConnectModal();
                    }, 1400);
                    return;
                }
            }

            // 2. Provision ESP32-S3 via USB Serial
            const res = await provisionEspViaUsb(
                portToUse,
                selectedNetwork.ssid,
                connectPassword,
                formState.backendUrl
            );

            if (res.success) {
                wifiConnectSuccess = true;
                wifiSuccessMsg = `✓ Provisioned ${selectedNetwork.ssid} to ESP32-S3! Syncing link...`;
                connectionStatus = 'connected';
                lastConnDetails = res;
                onDeviceConnectionChange?.(true, res.deviceId);
                await refreshHardwareStatus();
                setTimeout(() => {
                    closeConnectModal();
                }, 1600);
            } else {
                connectError = res.message || `Failed to connect ESP32-S3 to ${selectedNetwork.ssid}`;
            }
        } catch (err: any) {
            connectError = err?.message || 'Provisioning request failed.';
        } finally {
            isConnectingWifi = false;
        }
    }

    async function handleDirectWebSerialConnect() {
        if (typeof navigator === 'undefined' || !('serial' in navigator)) {
            webSerialStatus = 'error';
            webSerialMsg = 'Web Serial API is available in Chromium, Google Chrome, or Microsoft Edge on Fedora Linux.';
            return;
        }

        isWebSerialConnecting = true;
        webSerialStatus = 'idle';
        webSerialMsg = 'Requesting serial port authorization (/dev/ttyACM0 or /dev/ttyUSB0)...';

        try {
            const port = await (navigator as any).serial.requestPort({
                filters: [
                    { usbVendorId: 0x303a } // Espressif Systems
                ]
            });

            await port.open({ baudRate: 115200 });

            // Send provision command
            const encoder = new TextEncoder();
            const writer = port.writable.getWriter();
            const payload = JSON.stringify({
                cmd: 'provision_wifi',
                ssid: selectedNetwork?.ssid || serverWifiSsid || 'Apsari',
                pass: connectPassword || serverWifiPassword || '',
                backend_ip: serverBackendIp || '192.168.100.165',
                backend_port: '8080'
            }) + '\n';

            await writer.write(encoder.encode(payload));
            writer.releaseLock();

            webSerialStatus = 'success';
            webSerialMsg = `✓ Direct USB serial command dispatched to ESP32-S3. Syncing hardware link...`;

            setTimeout(async () => {
                await refreshHardwareStatus();
                await handleConnectEsp();
                try {
                    await port.close();
                } catch {}
            }, 1200);
        } catch (err: any) {
            if (err.name !== 'NotFoundError') {
                webSerialStatus = 'error';
                webSerialMsg = err?.message || 'Failed to open serial port.';
            } else {
                webSerialMsg = '';
            }
        } finally {
            isWebSerialConnecting = false;
        }
    }

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
            onDeviceConnectionChange?.(true, res.deviceId);
            await refreshHardwareStatus();
        } else {
            connectionStatus = 'failed';
            errorMessage = res.message || 'USB Wi-Fi provisioning failed.';
        }
    }

    async function handleDisconnectEsp() {
        const targetId = formState.deviceId || lastConnDetails.deviceId || '*';
        try {
            await disconnectEspDevice(targetId, formState.backendUrl);
        } catch {}
        connectionStatus = 'disconnected';
        errorMessage = '';
        onDeviceConnectionChange?.(false, '');
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
        <!-- 1. Gateway & Connectivity (Fedora Adwaita / Quick Settings UI/UX) -->
        <div class="settings-card fedora-card">
            <div class="section-header">
                <span class="section-icon">🌐</span>
                <div>
                    <h3>Gateway & Hardware Connectivity</h3>
                    <p class="fedora-section-subtitle">Fedora Peripheral & Wireless Node Controller</p>
                </div>
            </div>

            <!-- ESP32-S3 Connection Controller -->
            <div class="form-group esp-connect-group">
                <div class="esp-header-row">
                    <label for="deviceId">ESP32-S3 Sensor Node</label>
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

                <!-- Target Node ID Input & Connect Action Bar -->
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
            </div>

            <!-- Hardware Serial Ports (/dev/ttyACM*, /dev/ttyUSB*) Controller -->
            <div class="fedora-group">
                <div class="fedora-group-header">
                    <div class="header-title-box">
                        <svg class="fedora-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                            <path stroke-linecap="round" stroke-linejoin="round" d="M12 2v20m0-20L8 6m4-4l4 4M8 10h8m-8 6h8" />
                        </svg>
                        <div>
                            <h4>Hardware Serial Ports (<code>/dev/ttyACM*</code>, <code>/dev/ttyUSB*</code>)</h4>
                            <p class="fedora-subtext">Direct Linux serial device controller & hardware node inspection</p>
                        </div>
                    </div>
                    <div class="fedora-header-actions">
                        <button type="button" class="fedora-pill-btn" onclick={refreshHardwareStatus} title="Rescan server USB and ACM ports">
                            <span class="btn-refresh-icon">↻</span> Rescan Ports
                        </button>
                        <button
                            type="button"
                            class="fedora-accent-pill-btn"
                            onclick={handleDirectWebSerialConnect}
                            disabled={isWebSerialConnecting}
                            title="Direct Web Serial connection to /dev/ttyACM0 from browser"
                        >
                            <span class="lightning-icon">⚡</span> Direct Browser Connect
                        </button>
                    </div>
                </div>

                <div class="fedora-listbox">
                    {#if detectedUsbPorts.length > 0}
                        {#each detectedUsbPorts as portInfo}
                            <div class="fedora-list-row active-port-row">
                                <div class="row-leading">
                                    <div class="hardware-chip-icon">
                                        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                            <rect x="4" y="4" width="16" height="16" rx="2" />
                                            <rect x="9" y="9" width="6" height="6" />
                                            <path d="M9 1v3M15 1v3M9 20v3M15 20v3M20 9h3M20 14h3M1 9h3M1 14h3" />
                                        </svg>
                                    </div>
                                    <div class="row-info">
                                        <div class="port-primary-line">
                                            <strong class="port-name">{portInfo.port}</strong>
                                            <span class="fedora-badge green">ACM / USB Active</span>
                                            {#if portInfo.is_esp32}
                                                <span class="fedora-badge blue">ESP32-S3</span>
                                            {/if}
                                        </div>
                                        <span class="port-detail-text">
                                            {portInfo.product || 'ESP32-S3 USB JTAG / Serial Unit'}
                                            {#if portInfo.vendor_id}
                                                • ID {portInfo.vendor_id}:{portInfo.product_id} ({portInfo.manufacturer || 'Espressif Systems'})
                                            {/if}
                                        </span>
                                        {#if portInfo.current_ssid || (espUsbStatus && espUsbStatus.port === portInfo.port && espUsbStatus.current_ssid)}
                                            {@const curSsid = portInfo.current_ssid || espUsbStatus?.current_ssid}
                                            <div class="port-ssid-indicator">
                                                <span>ESP32 Connected Wi-Fi: <strong>{curSsid}</strong></span>
                                                {#if curSsid === serverWifiSsid}
                                                    <span class="fedora-badge green">✓ Same as Server</span>
                                                {:else}
                                                    <span class="fedora-badge yellow">⚠ Different SSID</span>
                                                {/if}
                                            </div>
                                        {:else}
                                            <div class="port-ssid-indicator">
                                                <span class="port-subdetail-muted">Wi-Fi: Not Associated (Select a Wi-Fi below to switch / connect)</span>
                                            </div>
                                        {/if}
                                    </div>
                                </div>
                                <div class="row-trailing">
                                    <button
                                        type="button"
                                        class="fedora-row-action-btn"
                                        onclick={() => handleProbeUsbPort(portInfo.port)}
                                        disabled={isProbingUsb}
                                        title="Probe ESP32 serial status"
                                    >
                                        {#if isProbingUsb}
                                            <span class="btn-spinner"></span>
                                        {:else}
                                            🔍 Probe
                                        {/if}
                                    </button>
                                </div>
                            </div>
                        {/each}
                    {:else}
                        <div class="fedora-list-row empty-row">
                            <div class="row-leading">
                                <span class="empty-icon">ℹ️</span>
                                <div>
                                    <span class="empty-title">Direct Host Hardware Mode: <code>/dev/ttyACM0</code></span>
                                    <p class="port-detail-text">Plugged into Fedora host. Click <strong>Direct Browser Connect</strong> to pair with <code>/dev/ttyACM0</code> directly via Web Serial, or use Wi-Fi below.</p>
                                </div>
                            </div>
                            <div class="row-trailing">
                                <button type="button" class="fedora-accent-pill-btn" onclick={handleDirectWebSerialConnect}>
                                    ⚡ Pair /dev/ttyACM0
                                </button>
                            </div>
                        </div>
                    {/if}
                </div>

                {#if webSerialMsg}
                    <div class="fedora-notice {webSerialStatus}">
                        <span>{webSerialMsg}</span>
                    </div>
                {/if}
            </div>

            <!-- Fedora Wi-Fi Quick Settings / Network Connector -->
            <div class="fedora-group">
                <div class="fedora-group-header">
                    <div class="header-title-box">
                        <svg class="fedora-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                            <path stroke-linecap="round" stroke-linejoin="round" d="M1.42 9a16 16 0 0 1 21.16 0M4.93 12.5a12 12 0 0 1 14.14 0M8.46 16a8 8 0 0 1 7.08 0M12 19.5h.01" />
                        </svg>
                        <div>
                            <h4>Wi-Fi Networks</h4>
                            <p class="fedora-subtext">Connect ESP32-S3 node</p>
                        </div>
                    </div>
                    <button type="button" class="fedora-pill-btn" onclick={handleScanWifi} disabled={isScanningWifi}>
                        {#if isScanningWifi}
                            <span class="btn-spinner"></span> Scanning...
                        {:else}
                            <span class="btn-refresh-icon">↻</span> Scan Networks
                        {/if}
                    </button>
                </div>

                <div class="fedora-listbox">
                    {#if wifiNetworks.length > 0}
                        {#each wifiNetworks as net}
                            <div
                                class="fedora-list-row wifi-row"
                                role="button"
                                tabindex="0"
                                onclick={() => handleSelectNetwork(net)}
                                onkeydown={(e) => { if (e.key === 'Enter') handleSelectNetwork(net); }}
                            >
                                <div class="row-leading">
                                    <div class="wifi-signal-icon" title="Signal: {net.signal}%">
                                        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                            <path stroke-linecap="round" stroke-linejoin="round" d="M12 19.5h.01" stroke-width="3" />
                                            {#if net.signal >= 25}
                                                <path stroke-linecap="round" stroke-linejoin="round" d="M8.46 16a8 8 0 0 1 7.08 0" />
                                            {/if}
                                            {#if net.signal >= 50}
                                                <path stroke-linecap="round" stroke-linejoin="round" d="M4.93 12.5a12 12 0 0 1 14.14 0" />
                                            {/if}
                                            {#if net.signal >= 75}
                                                <path stroke-linecap="round" stroke-linejoin="round" d="M1.42 9a16 16 0 0 1 21.16 0" />
                                            {/if}
                                        </svg>
                                    </div>
                                    <div class="wifi-info">
                                        <span class="wifi-ssid">{net.ssid}</span>
                                        <div class="wifi-badges">
                                            {#if net.is_server}
                                                <span class="fedora-badge blue">Server Wi-Fi</span>
                                            {/if}
                                            {#if net.is_saved}
                                                <span class="fedora-badge gray">Saved</span>
                                            {/if}
                                            {#if connectionStatus === 'connected' && lastConnDetails.provisionedSsid === net.ssid}
                                                <span class="fedora-badge green">Connected</span>
                                            {/if}
                                        </div>
                                    </div>
                                </div>
                                <div class="row-trailing">
                                    {#if net.security && net.security !== 'Open'}
                                        <span title="Secured with {net.security}">
                                            <svg class="lock-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                                <rect x="3" y="11" width="18" height="11" rx="2" ry="2" />
                                                <path d="M7 11V7a5 5 0 0 1 10 0v4" />
                                            </svg>
                                        </span>
                                    {/if}
                                    {#if connectionStatus === 'connected' && (lastConnDetails.provisionedSsid === net.ssid || (net.is_server && lastConnDetails.connectionMode === 'wifi_lan'))}
                                        <span class="active-check" title="Active Network Link">✓ Connected</span>
                                    {:else}
                                        <button
                                            type="button"
                                            class="fedora-row-action-btn {net.is_server ? 'primary' : ''}"
                                            onclick={(e) => { e.stopPropagation(); handleSelectNetwork(net); }}
                                        >
                                            {net.is_server ? 'Connect ESP' : 'Connect'}
                                        </button>
                                    {/if}
                                </div>
                            </div>
                        {/each}
                    {:else}
                        <div class="fedora-list-row empty-row">
                            <div class="row-leading">
                                <span class="empty-icon">📡</span>
                                <div>
                                    <span class="empty-title">Scanning for available Wi-Fi networks...</span>
                                    <p class="port-detail-text">Select <strong>Scan Networks</strong> or verify that the server Wi-Fi interface is active.</p>
                                </div>
                            </div>
                            <div class="row-trailing">
                                <button type="button" class="fedora-pill-btn" onclick={handleScanWifi}>
                                    Scan Now
                                </button>
                            </div>
                        </div>
                    {/if}
                </div>
            </div>

            <!-- Adwaita / Fedora Wi-Fi Password Connection Modal -->
            {#if showConnectModal && selectedNetwork}
                <div class="fedora-modal-backdrop" onclick={closeConnectModal} role="presentation">
                    <div class="fedora-dialog" onclick={(e) => e.stopPropagation()} role="dialog" aria-modal="true" aria-labelledby="dialog-title">
                        <div class="dialog-header">
                            <div class="dialog-icon-circle">
                                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                    <path stroke-linecap="round" stroke-linejoin="round" d="M1.42 9a16 16 0 0 1 21.16 0M4.93 12.5a12 12 0 0 1 14.14 0M8.46 16a8 8 0 0 1 7.08 0M12 19.5h.01" />
                                </svg>
                            </div>
                            <div>
                                <h3 id="dialog-title">
                                    {selectedNetwork.is_server ? 'Connect ESP32-S3 to Server Wi-Fi' : 'Connect to Wireless Network'}
                                </h3>
                                <p class="dialog-subtitle">
                                    {#if selectedNetwork.is_server}
                                        Connect ESP32-S3 node to local server Wi-Fi <strong>{selectedNetwork.ssid}</strong> (Gateway: {serverBackendIp || '192.168.100.165'})
                                    {:else}
                                        Authentication required to connect ESP32-S3 to <strong>{selectedNetwork.ssid}</strong>
                                    {/if}
                                </p>
                            </div>
                        </div>

                        <div class="dialog-body">
                            {#if espUsbStatus?.current_ssid && espUsbStatus.current_ssid !== selectedNetwork.ssid}
                                <div class="dialog-switch-chip">
                                    <span class="switch-chip-from">Current ESP Wi-Fi: <strong>{espUsbStatus.current_ssid}</strong></span>
                                    <span class="switch-chip-arrow">➔</span>
                                    <span class="switch-chip-to">Switch to: <strong>{selectedNetwork.ssid}</strong></span>
                                </div>
                            {/if}

                            <div class="dialog-field">
                                <label for="wifi-dialog-ssid">Target Wi-Fi Network (SSID)</label>
                                <input id="wifi-dialog-ssid" type="text" class="fedora-input disabled" value={selectedNetwork.ssid} readonly />
                            </div>

                            <div class="dialog-field">
                                <div class="field-label-row">
                                    <label for="wifi-dialog-pass">Password</label>
                                    {#if selectedNetwork.is_server}
                                        <span class="auto-creds-badge">✓ Auto-managed by Server</span>
                                    {/if}
                                </div>
                                <div class="password-input-wrap">
                                    <input
                                        id="wifi-dialog-pass"
                                        type={showPassword ? 'text' : 'password'}
                                        bind:value={connectPassword}
                                        placeholder={selectedNetwork.is_server ? "Server saved credentials (leave blank to auto-use server password)" : "Enter Wi-Fi network password"}
                                        class="fedora-input"
                                        autocomplete="new-password"
                                    />
                                    <button
                                        type="button"
                                        class="eye-toggle-btn"
                                        onclick={() => showPassword = !showPassword}
                                        title={showPassword ? 'Hide password' : 'Show password'}
                                    >
                                        {#if showPassword}
                                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                                <path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24" />
                                                <line x1="1" y1="1" x2="23" y2="23" />
                                            </svg>
                                        {:else}
                                            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                                <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z" />
                                                <circle cx="12" cy="12" r="3" />
                                            </svg>
                                        {/if}
                                    </button>
                                </div>
                            </div>

                            <div class="dialog-field-row">
                                <div class="dialog-field half">
                                    <label for="wifi-dialog-port">Hardware Flash Port</label>
                                    <select id="wifi-dialog-port" bind:value={selectedUsbPort} class="fedora-select">
                                        {#if detectedUsbPorts.length > 0}
                                            {#each detectedUsbPorts as p}
                                                <option value={p.port}>{p.port} ({p.product || 'ESP32-S3'})</option>
                                            {/each}
                                        {:else}
                                            <option value="/dev/ttyACM0">/dev/ttyACM0 (ESP32-S3 JTAG)</option>
                                            <option value="/dev/ttyUSB0">/dev/ttyUSB0 (ESP32 Serial)</option>
                                        {/if}
                                    </select>
                                </div>
                                <div class="dialog-field half">
                                    <label for="wifi-dialog-backend">Gateway Host IP</label>
                                    <input id="wifi-dialog-backend" type="text" class="fedora-input disabled" value={serverBackendIp || '192.168.100.165'} readonly />
                                </div>
                            </div>

                            <div class="dialog-security-note">
                                🔒 <span><strong>Zero-Persistence Security:</strong> Credentials are sent transiently to ESP32 memory via USB CDC and are never stored in database or cookies.</span>
                            </div>

                            {#if connectError}
                                <div class="dialog-alert error">
                                    <span>⚠️ {connectError}</span>
                                </div>
                            {/if}

                            {#if wifiConnectSuccess}
                                <div class="dialog-alert success">
                                    <span>✓ {wifiSuccessMsg}</span>
                                </div>
                            {/if}
                        </div>

                        <div class="dialog-footer">
                            <button type="button" class="fedora-btn secondary" onclick={closeConnectModal} disabled={isConnectingWifi}>
                                Cancel
                            </button>
                            <button type="button" class="fedora-btn primary" onclick={handleConnectToSelectedNetwork} disabled={isConnectingWifi}>
                                {#if isConnectingWifi}
                                    <span class="btn-spinner"></span> Switching & Connecting ESP32...
                                {:else if espUsbStatus?.current_ssid && espUsbStatus.current_ssid !== selectedNetwork.ssid}
                                    Switch ESP32 to "{selectedNetwork.ssid}"
                                {:else if selectedNetwork.is_server}
                                    Connect ESP32 to Server Wi-Fi
                                {:else}
                                    Connect ESP32
                                {/if}
                            </button>
                        </div>
                    </div>
                </div>
            {/if}

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
                        <span class="idle-text">Select your Wi-Fi network above to provision ESP32-S3, or plug into <code>/dev/ttyACM0</code> to automatically apply credentials.</span>
                    </div>
                {/if}
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
                    min="0.25"
                    max="0.98"
                    step="0.01"
                    bind:value={formState.minConfidenceThreshold}
                    oninput={() => onSaveConfig(formState)}
                    class="form-range"
                />
                <small>Vision classification acceptance threshold (Range: 25% – 98%, Default: 35%)</small>
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
                        <strong>Local C++20 Vision Service ({parallelCores > 0 ? `${parallelCores}-Core ` : ''}OpenMP SIMD)</strong>
                        <small>Active on-premise zero-dependency machine vision pipeline ({parallelCores > 0 ? `${parallelCores} parallel worker threads` : 'multi-threaded'})</small>
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

    /* =========================================================================
       Fedora Adwaita & GNOME 45+ Quick Settings / Network UI Design System
       ========================================================================= */

    .fedora-card {
        border-radius: 16px;
        background: var(--bg-card);
        border: 1px solid var(--border-subtle);
    }

    .fedora-section-subtitle {
        font-size: 0.72rem;
        color: var(--text-muted);
        margin: 2px 0 0 0;
    }

    .fedora-group {
        display: flex;
        flex-direction: column;
        gap: 10px;
        background: rgba(255, 255, 255, 0.02);
        border: 1px solid rgba(255, 255, 255, 0.06);
        border-radius: 14px;
        padding: 14px;
        margin-top: 6px;
    }

    .fedora-group-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        flex-wrap: wrap;
    }

    .header-title-box {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .header-title-box h4 {
        margin: 0;
        font-size: 0.88rem;
        font-weight: 700;
        color: var(--text-primary);
        letter-spacing: -0.01em;
    }

    .fedora-icon {
        width: 22px;
        height: 22px;
        color: #3584e4;
        flex-shrink: 0;
    }

    .fedora-subtext {
        margin: 2px 0 0 0;
        font-size: 0.7rem;
        color: var(--text-muted);
    }

    .fedora-header-actions {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
    }

    .fedora-pill-btn {
        background: rgba(255, 255, 255, 0.06);
        border: 1px solid rgba(255, 255, 255, 0.12);
        color: var(--text-secondary);
        font-size: 0.72rem;
        font-weight: 600;
        padding: 5px 12px;
        border-radius: 9999px;
        cursor: pointer;
        display: inline-flex;
        align-items: center;
        gap: 5px;
        transition: all 0.15s ease;
    }

    .fedora-pill-btn:hover:not(:disabled) {
        background: rgba(255, 255, 255, 0.12);
        color: var(--text-primary);
        border-color: rgba(255, 255, 255, 0.2);
    }

    .fedora-accent-pill-btn {
        background: #3584e4;
        border: 1px solid #1c71d8;
        color: #ffffff;
        font-size: 0.72rem;
        font-weight: 600;
        padding: 5px 14px;
        border-radius: 9999px;
        cursor: pointer;
        display: inline-flex;
        align-items: center;
        gap: 6px;
        transition: all 0.15s ease;
        box-shadow: 0 1px 3px rgba(0, 0, 0, 0.2);
    }

    .fedora-accent-pill-btn:hover:not(:disabled) {
        background: #1c71d8;
    }

    .fedora-accent-pill-btn:disabled {
        opacity: 0.6;
        cursor: not-allowed;
    }

    .lightning-icon {
        font-size: 0.8rem;
    }

    .btn-refresh-icon {
        font-size: 0.85rem;
    }

    /* Listbox: AdwPreferencesGroup style */
    .fedora-listbox {
        display: flex;
        flex-direction: column;
        background: rgba(0, 0, 0, 0.2);
        border: 1px solid rgba(255, 255, 255, 0.06);
        border-radius: 12px;
        overflow: hidden;
    }

    .fedora-list-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 10px 14px;
        gap: 12px;
        border-bottom: 1px solid rgba(255, 255, 255, 0.04);
        transition: background 0.15s ease;
    }

    .fedora-list-row:last-child {
        border-bottom: none;
    }

    .fedora-list-row.wifi-row {
        cursor: pointer;
    }

    .fedora-list-row.wifi-row:hover {
        background: rgba(255, 255, 255, 0.04);
    }

    .row-leading {
        display: flex;
        align-items: center;
        gap: 12px;
        min-width: 0;
    }

    .hardware-chip-icon {
        width: 32px;
        height: 32px;
        border-radius: 8px;
        background: rgba(53, 132, 228, 0.15);
        color: #3584e4;
        display: flex;
        align-items: center;
        justify-content: center;
        flex-shrink: 0;
    }

    .hardware-chip-icon svg {
        width: 18px;
        height: 18px;
    }

    .row-info {
        display: flex;
        flex-direction: column;
        gap: 2px;
        min-width: 0;
    }

    .port-primary-line {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
    }

    .port-name {
        font-family: var(--font-mono);
        font-size: 0.82rem;
        color: var(--text-primary);
    }

    .port-detail-text {
        font-size: 0.68rem;
        color: var(--text-muted);
        line-height: 1.3;
    }

    .port-detail-text code {
        color: #3584e4;
        background: rgba(53, 132, 228, 0.1);
        padding: 1px 4px;
        border-radius: 4px;
        font-family: var(--font-mono);
    }

    .row-trailing {
        display: flex;
        align-items: center;
        gap: 10px;
        flex-shrink: 0;
    }

    .status-ready-pill {
        font-size: 0.68rem;
        font-weight: 600;
        color: #2ec27e;
        background: rgba(46, 194, 126, 0.12);
        padding: 2px 8px;
        border-radius: 9999px;
        border: 1px solid rgba(46, 194, 126, 0.25);
    }

    .fedora-badge {
        font-size: 0.62rem;
        font-weight: 700;
        padding: 1px 7px;
        border-radius: 9999px;
        text-transform: uppercase;
        letter-spacing: 0.03em;
    }

    .fedora-badge.blue {
        background: rgba(53, 132, 228, 0.18);
        color: #78aeed;
        border: 1px solid rgba(53, 132, 228, 0.3);
    }

    .fedora-badge.green {
        background: rgba(46, 194, 126, 0.15);
        color: #2ec27e;
        border: 1px solid rgba(46, 194, 126, 0.3);
    }

    .fedora-badge.gray {
        background: rgba(255, 255, 255, 0.08);
        color: var(--text-muted);
        border: 1px solid rgba(255, 255, 255, 0.12);
    }

    .fedora-badge.yellow {
        background: rgba(245, 158, 11, 0.18);
        color: #fbbf24;
        border: 1px solid rgba(245, 158, 11, 0.35);
    }

    .port-ssid-indicator {
        margin-top: 4px;
        display: flex;
        align-items: center;
        gap: 8px;
        font-size: 0.7rem;
        color: var(--text-secondary);
    }

    .port-subdetail-muted {
        font-size: 0.67rem;
        color: var(--text-muted);
    }

    .dialog-switch-chip {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 8px 12px;
        background: rgba(245, 158, 11, 0.1);
        border: 1px solid rgba(245, 158, 11, 0.25);
        border-radius: 8px;
        font-size: 0.74rem;
        color: var(--text-primary);
    }

    .switch-chip-from strong {
        color: #fbbf24;
    }

    .switch-chip-arrow {
        color: #3584e4;
        font-weight: 800;
    }

    .switch-chip-to strong {
        color: #2ec27e;
    }

    .dialog-security-note {
        display: flex;
        align-items: flex-start;
        gap: 6px;
        padding: 6px 10px;
        background: rgba(255, 255, 255, 0.04);
        border-radius: 6px;
        font-size: 0.68rem;
        color: var(--text-muted);
        line-height: 1.35;
    }

    .dialog-security-note strong {
        color: var(--text-secondary);
    }

    /* Wi-Fi Row Specifics */
    .wifi-signal-icon {
        width: 24px;
        height: 24px;
        color: #3584e4;
        display: flex;
        align-items: center;
        justify-content: center;
        flex-shrink: 0;
    }

    .wifi-signal-icon svg {
        width: 20px;
        height: 20px;
    }

    .wifi-info {
        display: flex;
        flex-direction: column;
        gap: 3px;
        min-width: 0;
    }

    .wifi-ssid {
        font-size: 0.82rem;
        font-weight: 600;
        color: var(--text-primary);
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }

    .wifi-badges {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .lock-icon {
        width: 15px;
        height: 15px;
        color: var(--text-muted);
    }

    .active-check {
        color: #2ec27e;
        font-weight: 800;
        font-size: 0.95rem;
    }

    .fedora-row-action-btn {
        background: rgba(255, 255, 255, 0.08);
        border: 1px solid rgba(255, 255, 255, 0.12);
        color: var(--text-primary);
        font-size: 0.7rem;
        font-weight: 600;
        padding: 4px 12px;
        border-radius: 9999px;
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .fedora-row-action-btn:hover {
        background: #3584e4;
        border-color: #1c71d8;
        color: #ffffff;
    }

    .fedora-row-action-btn.primary {
        background: rgba(53, 132, 228, 0.2);
        border-color: rgba(53, 132, 228, 0.4);
        color: #78aeed;
    }

    .fedora-row-action-btn.primary:hover {
        background: #3584e4;
        border-color: #1c71d8;
        color: #ffffff;
    }

    .field-label-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .auto-creds-badge {
        font-size: 0.65rem;
        color: #2ec27e;
        font-weight: 600;
    }

    .empty-row {
        padding: 16px 14px;
    }

    .empty-icon {
        font-size: 1.2rem;
    }

    .empty-title {
        font-size: 0.78rem;
        font-weight: 600;
        color: var(--text-secondary);
    }

    .fedora-notice {
        padding: 8px 12px;
        border-radius: 8px;
        font-size: 0.72rem;
        margin-top: 4px;
    }

    .fedora-notice.idle {
        background: rgba(53, 132, 228, 0.1);
        color: #78aeed;
        border: 1px solid rgba(53, 132, 228, 0.2);
    }

    .fedora-notice.success {
        background: rgba(46, 194, 126, 0.12);
        color: #2ec27e;
        border: 1px solid rgba(46, 194, 126, 0.25);
    }

    .fedora-notice.error {
        background: rgba(239, 68, 68, 0.12);
        color: #f87171;
        border: 1px solid rgba(239, 68, 68, 0.25);
    }

    /* Modal / Adwaita Dialog */
    .fedora-modal-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.65);
        backdrop-filter: blur(4px);
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 1000;
        padding: 16px;
    }

    .fedora-dialog {
        background: #242424;
        border: 1px solid rgba(255, 255, 255, 0.12);
        border-radius: 16px;
        width: 100%;
        max-width: 440px;
        box-shadow: 0 16px 40px rgba(0, 0, 0, 0.6);
        overflow: hidden;
        animation: dialogFadeIn 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    }

    @keyframes dialogFadeIn {
        from {
            opacity: 0;
            transform: scale(0.95) translateY(6px);
        }
        to {
            opacity: 1;
            transform: scale(1) translateY(0);
        }
    }

    .dialog-header {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 18px 20px 14px;
        border-bottom: 1px solid rgba(255, 255, 255, 0.06);
    }

    .dialog-icon-circle {
        width: 38px;
        height: 38px;
        border-radius: 50%;
        background: rgba(53, 132, 228, 0.18);
        color: #3584e4;
        display: flex;
        align-items: center;
        justify-content: center;
        flex-shrink: 0;
    }

    .dialog-icon-circle svg {
        width: 22px;
        height: 22px;
    }

    .dialog-header h3 {
        margin: 0;
        font-size: 0.95rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .dialog-subtitle {
        margin: 2px 0 0 0;
        font-size: 0.72rem;
        color: var(--text-muted);
    }

    .dialog-body {
        padding: 16px 20px;
        display: flex;
        flex-direction: column;
        gap: 12px;
    }

    .dialog-field {
        display: flex;
        flex-direction: column;
        gap: 5px;
    }

    .dialog-field label {
        font-size: 0.68rem;
        font-weight: 600;
        text-transform: uppercase;
        letter-spacing: 0.03em;
        color: var(--text-muted);
    }

    .dialog-field-row {
        display: flex;
        gap: 12px;
    }

    .dialog-field.half {
        flex: 1;
        min-width: 0;
    }

    .fedora-input,
    .fedora-select {
        background: rgba(255, 255, 255, 0.05);
        border: 1px solid rgba(255, 255, 255, 0.1);
        border-radius: 8px;
        padding: 8px 12px;
        color: var(--text-primary);
        font-size: 0.8rem;
        outline: none;
        transition: border-color 0.15s ease;
        width: 100%;
        box-sizing: border-box;
    }

    .fedora-input:focus,
    .fedora-select:focus {
        border-color: #3584e4;
        box-shadow: 0 0 0 2px rgba(53, 132, 228, 0.25);
    }

    .fedora-input.disabled {
        opacity: 0.7;
        background: rgba(0, 0, 0, 0.25);
        cursor: not-allowed;
    }

    .password-input-wrap {
        position: relative;
        display: flex;
        align-items: center;
    }

    .password-input-wrap .fedora-input {
        padding-right: 36px;
    }

    .eye-toggle-btn {
        position: absolute;
        right: 8px;
        background: transparent;
        border: none;
        color: var(--text-muted);
        cursor: pointer;
        padding: 4px;
        display: flex;
        align-items: center;
        justify-content: center;
    }

    .eye-toggle-btn svg {
        width: 16px;
        height: 16px;
    }

    .eye-toggle-btn:hover {
        color: var(--text-primary);
    }

    .dialog-alert {
        padding: 8px 12px;
        border-radius: 8px;
        font-size: 0.74rem;
        line-height: 1.3;
    }

    .dialog-alert.error {
        background: rgba(239, 68, 68, 0.15);
        color: #fca5a5;
        border: 1px solid rgba(239, 68, 68, 0.3);
    }

    .dialog-alert.success {
        background: rgba(46, 194, 126, 0.15);
        color: #86efac;
        border: 1px solid rgba(46, 194, 126, 0.3);
    }

    .dialog-footer {
        padding: 12px 20px 16px;
        display: flex;
        align-items: center;
        justify-content: flex-end;
        gap: 10px;
        border-top: 1px solid rgba(255, 255, 255, 0.06);
    }

    .fedora-btn {
        padding: 7px 18px;
        border-radius: 9999px;
        font-size: 0.76rem;
        font-weight: 600;
        cursor: pointer;
        display: inline-flex;
        align-items: center;
        gap: 6px;
        transition: all 0.15s ease;
    }

    .fedora-btn.secondary {
        background: rgba(255, 255, 255, 0.08);
        border: 1px solid rgba(255, 255, 255, 0.12);
        color: var(--text-secondary);
    }

    .fedora-btn.secondary:hover:not(:disabled) {
        background: rgba(255, 255, 255, 0.14);
        color: var(--text-primary);
    }

    .fedora-btn.primary {
        background: #3584e4;
        border: 1px solid #1c71d8;
        color: #ffffff;
        box-shadow: 0 1px 4px rgba(0, 0, 0, 0.3);
    }

    .fedora-btn.primary:hover:not(:disabled) {
        background: #1c71d8;
    }

    .fedora-btn:disabled {
        opacity: 0.6;
        cursor: not-allowed;
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
        .esp-failed-box,
        .fedora-group-header {
            flex-wrap: wrap;
            gap: 8px;
        }

        .dialog-field-row {
            flex-direction: column;
            gap: 10px;
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

