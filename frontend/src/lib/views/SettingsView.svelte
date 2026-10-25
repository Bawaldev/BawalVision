<script lang="ts">
    import { onMount } from 'svelte';
    import { connectEspDevice, disconnectEspDevice, fetchDeviceStatus, provisionEspViaUsb, fetchWiFiNetworks, probeEspUsbPort } from '../api';
    import { detectLocalBackend, getConfiguredApiBase, resolveApiBase, setConfiguredApiBase } from '../backendConfig';
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
        backendUrl: getConfiguredApiBase(),
        deviceId: '',
        streamIntervalMs: 1500,
        enableMockIfOffline: true,
        minConfidenceThreshold: 0.35,
        gravitonEngineEnabled: true,
        localEngineEnabled: true
    });

    // Global inspection source preference (shared with CameraStream).
    type InspectionSourceMode = 'live' | 'mock';
    let inspectionSource = $state<InspectionSourceMode>('live');
    let sourceModeBusy = $state(false);

    // ESP32-S3 Hardware Connection & USB Serial Provisioning State
    let connectionStatus = $state<'disconnected' | 'connecting' | 'connected' | 'wifi_only' | 'failed'>('disconnected');
    let connectingStepText = $state('Probing shared Wi-Fi & USB slots...');
    let errorMessage = $state('');
    let detectedUsbPorts = $state<USBPortInfo[]>([]);
    let selectedUsbPort = $state('');
    let backendOnline = $state(true);
    let detectingLocalBackend = $state(false);
    let localBackendMessage = $state('');
    let portOptions = $derived.by(() => detectedUsbPorts.map((p) => p.port));
    let detectedHostIp = $state('');
    let gatewayHostIp = $state('');
    let gatewayHostIpEdited = $state(false);
    let selectedSsid = $state('');
    let connectToast = $state('');
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

    function isDockerOrLoopbackIp(raw: string): boolean {
        const ip = raw.trim();
        if (!ip) return false;
        if (ip === 'localhost' || ip.startsWith('127.')) return true;
        const parts = ip.split('.').map(Number);
        if (parts.length !== 4 || parts.some((n) => !Number.isInteger(n) || n < 0 || n > 255)) return false;
        return parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31;
    }

    function isValidGatewayIp(raw: string): boolean {
        const ip = raw.trim();
        if (!ip) return true;
        const parts = ip.split('.').map(Number);
        if (parts.length !== 4 || parts.some((n) => !Number.isInteger(n) || n < 0 || n > 255)) return false;
        if (parts[0] === 0 || parts[0] === 127 || parts[0] >= 224) return false;
        if (parts[0] === 169 && parts[1] === 254) return false;
        if (isDockerOrLoopbackIp(ip)) {
            // Allow 172.x only when the UI is served from that address, which
            // means it is the host's real LAN IP rather than a Docker bridge.
            return typeof window !== 'undefined' && window.location.hostname === ip;
        }
        return true;
    }

    function isUsableLanHost(raw: string): boolean {
        const ip = raw.trim();
        return !!ip && ip !== 'localhost' && isValidGatewayIp(ip);
    }

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
    let pairingPort = $state('');
    let pairingGraceUntil = 0;

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
        if (!hw) {
            backendOnline = false;
            detectedUsbPorts = [];
            selectedUsbPort = '';
            wifiNetworks = [];
            return;
        }
        backendOnline = true;

        detectedUsbPorts = hw.usb_ports || [];
        const preferredPort =
            detectedUsbPorts.find((p) => p.is_esp32) || detectedUsbPorts[0];
        if (detectedUsbPorts.length === 0) {
            selectedUsbPort = isWebSerialAvailable() ? 'web_serial' : '';
        } else if (!selectedUsbPort || !detectedUsbPorts.some((p) => p.port === selectedUsbPort)) {
            selectedUsbPort = preferredPort?.port || '';
        }

        if (hw.wifi_networks && hw.wifi_networks.length > 0) {
            wifiNetworks = hw.wifi_networks;
        }

        if (hw.server_wifi) {
            // Track the backend's live association so the "Same as Server"
            // badge follows network switches (env WIFI_SSID may be stale).
            const backendSsid = hw.server_wifi.ssid || hw.server_wifi.active_ssid || '';
            if (backendSsid) {
                serverWifiSsid = backendSsid;
            }
            if (hw.server_wifi.backend_ip) {
                serverBackendIp = hw.server_wifi.backend_ip;
            }
        }

        // Prefer the address the browser used to reach the UI (if it is a real
        // LAN IP), then the backend's detected host IP. Never auto-fill a
        // Docker bridge/loopback address.
        const browserHost = typeof window !== 'undefined' ? window.location.hostname : '';
        const liveHostIp = isUsableLanHost(browserHost)
            ? browserHost
            : (hw.detected_host_ip || hw.server_wifi?.detected_host_ip || hw.server_wifi?.backend_ip || '');
        if (liveHostIp && isValidGatewayIp(liveHostIp)) {
            detectedHostIp = liveHostIp;
            // Auto mode: the gateway field tracks the host's current LAN IP, so
            // roaming the laptop to another Wi-Fi can never leave the previous
            // network's address behind (which strands the ESP32 on UDP
            // discovery and looks like an unstable connection). A value the
            // user typed manually is always preserved.
            if (!gatewayHostIpEdited || !gatewayHostIp || !isValidGatewayIp(gatewayHostIp)) {
                gatewayHostIp = liveHostIp;
            }
        } else {
            detectedHostIp = '';
            if (isDockerOrLoopbackIp(gatewayHostIp)) {
                gatewayHostIp = '';
            }
        }

        if (hw.esp_usb_status) {
            espUsbStatus = hw.esp_usb_status;
        }

        const inPairingGrace = Date.now() < pairingGraceUntil;
        const serialWifi = hw.esp_usb_status?.wifi_connected === true;
        if (hw.connected && !inPairingGrace) {
            connectionStatus = 'connected';
            lastConnDetails = {
                success: true,
                deviceId: hw.device_id || hw.esp_usb_status?.device_id || formState.deviceId,
                firmwareVersion: hw.firmware_version || '1.0.0',
                ipAddress: hw.ip_address || serverBackendIp || 'LAN',
                connectionMode: hw.connection_mode,
                latencyMs: hw.connection_mode === 'usb_serial' ? 4 : 9,
                message: hw.connection_mode === 'usb_serial'
                    ? `Connected via USB Serial (${hw.ip_address}) — Server Wi-Fi Synced`
                    : `Connected on shared Wi-Fi network (${hw.ip_address || 'LAN'})`
            };
        } else if (serialWifi && !inPairingGrace) {
            connectionStatus = 'wifi_only';
            lastConnDetails = {
                success: false,
                deviceId: hw.esp_usb_status?.device_id || formState.deviceId,
                firmwareVersion: hw.firmware_version || '1.0.0',
                ipAddress: hw.esp_usb_status?.port || 'USB Serial',
                connectionMode: 'usb_serial',
                latencyMs: 4,
                message: `ESP32-S3 joined Wi-Fi "${hw.esp_usb_status?.current_ssid || 'unknown'}", but the backend cannot reach it. Check AP client isolation or firewall.`
            };
        } else if (!hw.connected && !serialWifi &&
                   (connectionStatus === 'connected' || connectionStatus === 'wifi_only') &&
                   !inPairingGrace) {
            connectionStatus = 'disconnected';
        }
    }

    async function handleDetectLocalBackend() {
        detectingLocalBackend = true;
        localBackendMessage = 'Searching for the local C++ backend...';
        const isHttps = typeof window !== 'undefined' && window.location.protocol === 'https:';
        try {
            const detected = await detectLocalBackend(1800);
            if (detected) {
                formState.backendUrl = detected;
                localBackendMessage = `✓ Local live backend connected: ${detected}`;
                await refreshHardwareStatus();
            } else {
                formState.backendUrl = getConfiguredApiBase();
                if (isHttps) {
                    localBackendMessage =
                        'Browser HTTPS memblokir probe ke localhost HTTP. Buka frontend lokal di http://localhost:5173 (jalankan "just run-frontend"), atau masukkan LAN IP laptop.';
                } else {
                    localBackendMessage =
                        'Tidak ditemukan backend C++ di port 8080. Jalankan backend lokal dengan "just compose-up bawal_backend" atau "just run-backend" di terminal.';
                }
            }
        } finally {
            detectingLocalBackend = false;
        }
    }

    async function applyLiveBackendUrl() {
        formState.backendUrl = setConfiguredApiBase(formState.backendUrl);
        onSaveConfig({ ...formState });
        localBackendMessage = `Live backend: ${formState.backendUrl}`;
        await refreshHardwareStatus();
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
        try {
            const saved = localStorage.getItem('bawal_source_mode');
            if (saved === 'mock' || saved === 'live') {
                inspectionSource = saved;
            }
        } catch {
            // localStorage unavailable; keep the live default.
        }
        refreshHardwareStatus();
        void handleScanWifi();
        // One passive serial probe on load so the port card shows the board's
        // real SSID without pressing Probe (also warms the backend ACK cache).
        const probeTimer = setTimeout(() => {
            if (detectedUsbPorts.length > 0) void handleProbeUsbPort();
        }, 1800);
        const timer = setInterval(() => {
            if (!isWebSerialConnecting) {
                refreshHardwareStatus();
            }
        }, 1500);
        return () => {
            clearInterval(timer);
            clearTimeout(probeTimer);
        };
    });

    async function applyInspectionSource(mode: InspectionSourceMode) {
        if (mode === inspectionSource || sourceModeBusy) return;
        sourceModeBusy = true;
        inspectionSource = mode;
        try {
            localStorage.setItem('bawal_source_mode', mode);
        } catch {
            // Preference persistence is best-effort.
        }
        await fetch(`${resolveApiBase()}/camera/mode`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mode })
        }).catch(() => {});
        if (typeof window !== 'undefined') {
            window.dispatchEvent(new CustomEvent('bawal-source-mode', { detail: mode }));
        }
        sourceModeBusy = false;
    }

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
        selectedSsid = net.ssid;
        if (net.is_server || net.ssid === serverWifiSsid) {
            connectPassword = serverWifiPassword;
        } else {
            connectPassword = '';
        }
        connectError = '';
        wifiConnectSuccess = false;
        wifiSuccessMsg = '';
        showConnectModal = true;
        // Refresh the USB slot list every time the modal opens so the
        // Hardware Flash Port selector never renders empty.
        void refreshHardwareStatus();
    }

    function handleOpenManualWifiModal() {
        selectedNetwork = {
            ssid: '',
            is_server: false,
            signal: 100,
            security: 'WPA2'
        };
        selectedSsid = '';
        connectPassword = '';
        connectError = '';
        wifiConnectSuccess = false;
        wifiSuccessMsg = '';
        showConnectModal = true;
        void refreshHardwareStatus();
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

        const ssid = (selectedSsid || selectedNetwork.ssid || '').trim();
        const detectedPort = detectedUsbPorts[0]?.port || '';
        let gatewayIpToUse = (gatewayHostIp || detectedHostIp || '').trim();
        if (!gatewayIpToUse && serverBackendIp && isValidGatewayIp(serverBackendIp)) {
            gatewayIpToUse = serverBackendIp.trim();
        }
        if (gatewayIpToUse && !isValidGatewayIp(gatewayIpToUse)) {
            connectError =
                'Invalid gateway IP (Docker/loopback). Use host Wi-Fi IP, or leave blank so the ESP discovers the backend via UDP.';
            isConnectingWifi = false;
            return;
        }

        try {
            // Prefer direct browser Web Serial when explicitly selected or when
            // backend runs in VM/cloud without visible host USB ports.
            let res: EspConnectionResult;
            const useBrowserSerial = selectedUsbPort === 'web_serial' || (!detectedPort && isWebSerialAvailable());
            if (useBrowserSerial) {
                res = await provisionViaBrowserSerial(ssid, connectPassword, gatewayIpToUse);
            } else if (detectedPort) {
                res = await provisionEspViaUsb(
                    detectedPort,
                    ssid,
                    connectPassword,
                    formState.backendUrl,
                    gatewayIpToUse
                );
                // Transport-level server failures (no node visible to the
                // backend, port busy, ACK timeout) fall back to Web Serial;
                // credential rejections are surfaced as-is.
                if (!res.success && isWebSerialAvailable() && /not accessible|no such file|no USB serial|cannot open|timed out|permission denied|did not report|no_server_usb/i.test(res.message || '')) {
                    res = await provisionViaBrowserSerial(ssid, connectPassword, gatewayIpToUse);
                }
            } else {
                res = await provisionViaBrowserSerial(ssid, connectPassword, gatewayIpToUse);
            }

            if (res.success) {
                const targetIp = res.gatewayHostIp || gatewayIpToUse || 'auto';
                wifiConnectSuccess = true;
                wifiSuccessMsg = `ESP32 Connected to ${ssid} targeting ${targetIp}`;
                connectionStatus = 'connected';
                lastConnDetails = res;
                connectToast = `✓ ${wifiSuccessMsg}`;
                setTimeout(() => (connectToast = ''), 6000);
                onDeviceConnectionChange?.(true, res.deviceId);
                await refreshHardwareStatus();
                setTimeout(() => {
                    closeConnectModal();
                }, 1600);
            } else {
                connectError = res.message || `Failed to connect ESP32-S3 to ${ssid}`;
            }
        } catch (err: any) {
            connectError = err?.message || 'Provisioning request failed.';
        } finally {
            isConnectingWifi = false;
        }
    }

    function portLabel(port: string): string {
        const info = detectedUsbPorts.find((p) => p.port === port);
        if (info?.product) {
            return `${port} (${info.product})`;
        }
        if (port.startsWith('/dev/ttyACM')) return `${port} (ESP32-S3 JTAG / Serial)`;
        return `${port} (USB Serial)`;
    }

    function applyProvisionedResult(res: EspConnectionResult, port: string, ssid: string) {
        connectionStatus = 'connected';
        lastConnDetails = res;
        pairingGraceUntil = Date.now() + 12000;
        webSerialStatus = 'success';
        webSerialMsg = res.wifiConnected === false
            ? `✓ Credentials stored on ${port}. ESP32-S3 is associating with ${ssid}...`
            : `✓ ${port} paired to ${ssid}. ESP32-S3 is in sync.`;
        // Reflect the just-provisioned network immediately instead of waiting
        // for the next status poll to refresh the port card.
        espUsbStatus = {
            port,
            device_id: res.deviceId || espUsbStatus?.device_id || formState.deviceId || '',
            current_ssid: ssid,
            wifi_connected: res.wifiConnected !== false,
            temperature: espUsbStatus?.temperature ?? 0
        };
        onDeviceConnectionChange?.(true, res.deviceId);
    }

    async function readBawalAck(port: any, timeoutMs = 4000): Promise<any | null> {
        let reader: any = null;
        let timer: any = null;
        try {
            if (!port?.readable) return null;
            reader = port.readable.getReader();
            const decoder = new TextDecoder();
            let buffer = '';
            timer = setTimeout(() => {
                reader?.cancel().catch(() => {});
            }, timeoutMs);

            while (true) {
                const { value, done } = await reader.read();
                if (done) break;
                if (value) {
                    buffer += decoder.decode(value, { stream: true });
                }
                const ackIndex = buffer.indexOf('BAWAL_ACK:');
                if (ackIndex >= 0) {
                    const lineEnd = buffer.indexOf('\n', ackIndex);
                    if (lineEnd >= 0) {
                        const jsonText = buffer.slice(ackIndex + 'BAWAL_ACK:'.length, lineEnd).trim();
                        try {
                            return JSON.parse(jsonText);
                        } catch {
                            return null;
                        }
                    }
                }
                if (buffer.length > 8192) {
                    buffer = buffer.slice(-4096);
                }
            }
        } catch {
            return null;
        } finally {
            if (timer) clearTimeout(timer);
            try {
                reader?.releaseLock();
            } catch {}
        }
        return null;
    }

    function withTimeout<T>(promise: Promise<T>, timeoutMs: number): Promise<T> {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(
                () => reject(new Error('Timed out waiting for browser serial authorization')),
                timeoutMs
            );
            promise.then(
                (value) => {
                    clearTimeout(timer);
                    resolve(value);
                },
                (err) => {
                    clearTimeout(timer);
                    reject(err);
                }
            );
        });
    }

    function isWebSerialAvailable(): boolean {
        return typeof navigator !== 'undefined' && 'serial' in navigator;
    }

    function wifiProvisionErrorMessage(ack: any, ssid: string): string {
        const status = ack?.status || 'unknown';
        const connected = ack?.wifi_connected ? 'board Wi-Fi is connected' : 'board Wi-Fi is still offline';
        return `ESP32-S3 could not join "${ssid}" (status '${status}', ${connected}). ` +
            'Check the password and make sure the access point broadcasts on 2.4 GHz, then retry.';
    }

    // Direct browser-to-board provisioning. Essential when the backend runs in
    // Docker Desktop, where the daemon VM cannot see host /dev/ttyACM* nodes.
    async function provisionViaBrowserSerial(
        ssid: string,
        password: string,
        gatewayIp: string
    ): Promise<EspConnectionResult> {
        const failure = (message: string): EspConnectionResult => ({
            success: false,
            deviceId: '',
            connectionMode: 'disconnected',
            message
        });

        if (!isWebSerialAvailable()) {
            return failure(
                'The backend cannot see host USB serial (Docker Desktop runs in a VM), and Web Serial is unavailable. ' +
                    'Open this page in Chrome/Edge on localhost, or run the backend on the host with `just edge-stack`.'
            );
        }
        if (!ssid) {
            return failure('Target Wi-Fi SSID is required.');
        }

        let port: any = null;
        try {
            port = await withTimeout(
                (navigator as any).serial.requestPort({
                    filters: [{ usbVendorId: 0x303a }] // Espressif Systems
                }),
                20000
            );
        } catch (err: any) {
            return failure(
                err?.name === 'NotFoundError'
                    ? 'Serial port selection canceled.'
                    : err?.message || 'Unable to request USB serial access.'
            );
        }

        try {
            await port.open({ baudRate: 115200 });
            // Opening the CDC port asserts DTR and resets the ESP32-S3; give
            // the firmware time to boot its provisioning task before writing.
            await new Promise((resolve) => setTimeout(resolve, 3500));

            const encoder = new TextEncoder();
            const writer = port.writable.getWriter();
            const payload =
                JSON.stringify({
                    cmd: 'set_wifi',
                    ssid,
                    pass: password || '',
                    backend_ip: gatewayIp,
                    backend_port: 8080
                }) + '\n';

            await writer.write(encoder.encode(payload));
            writer.releaseLock();

            // The firmware answers only after it applied the credentials and
            // waited up to 20 s for the association (rollback on failure), so
            // a 7 s ACK window times out on every slow AP.
            const ack = await readBawalAck(port, 30000);
            if (!ack || (ack.status !== 'wifi_provisioned' && ack.status !== 'ok')) {
                return failure(
                    ack
                        ? wifiProvisionErrorMessage(ack, ssid)
                        : 'No BAWAL_ACK received from ESP32-S3. Close idf.py monitor or any other serial tool and retry.'
                );
            }

            const targetSsid = ack.ssid || ssid;
            return {
                success: true,
                status: ack.status,
                deviceId: ack.device_id || espUsbStatus?.device_id || formState.deviceId || 'ESP32S3_USB',
                firmwareVersion: ack.firmware_version || '1.0.0',
                latencyMs: 4,
                ipAddress: ack.backend_ip || gatewayIp || 'USB Serial',
                usbPort: 'Web Serial',
                provisionedSsid: targetSsid,
                gatewayHostIp: gatewayIp || ack.backend_ip || '',
                wifiConnected: !!ack.wifi_connected,
                connectionMode: 'usb_serial',
                message: `Provisioned ${targetSsid} via Web Serial`
            };
        } catch (err: any) {
            return failure(err?.message || 'Failed to provision Wi-Fi over Web Serial.');
        } finally {
            if (port) {
                try {
                    await port.close();
                } catch {}
            }
        }
    }

    // Server-side fallback: the backend opens /dev/ttyACM* directly, so it
    // works even when the browser lacks Web Serial or the user denies access.
    async function provisionViaServer(targetPort: string, ssidOverride?: string): Promise<boolean> {
        webSerialStatus = 'idle';
        webSerialMsg = `Web Serial is unavailable. Falling back to server-side USB provisioning on ${targetPort}...`;

        const hw = await fetchDeviceStatus(formState.backendUrl);
        if (!hw) {
            backendOnline = false;
            webSerialStatus = 'error';
            webSerialMsg =
                `Backend unreachable at ${formState.backendUrl || 'the configured URL'}. ` +
                'Start it on the host with "just run-backend" (not inside Docker Desktop), then retry.';
            return false;
        }
        backendOnline = true;
        if (hw.server_wifi) {
            if (hw.server_wifi.ssid) serverWifiSsid = hw.server_wifi.ssid;
            if (hw.server_wifi.backend_ip) serverBackendIp = hw.server_wifi.backend_ip;
        }

        const visiblePorts = hw.usb_ports || [];
        if (visiblePorts.length === 0) {
            webSerialStatus = 'idle';
            webSerialMsg =
                '💡 Cloud backend cannot see local laptop USB directly. ' +
                'Click "⚡ Direct Browser Connect" above to connect your ESP32-S3 (/dev/ttyACM0) natively via Web Serial in Chrome/Edge!';
            return false;
        }
        if (!visiblePorts.some((p) => p.port === targetPort)) {
            targetPort = visiblePorts[0].port;
        }

        const ssid = (ssidOverride || hw?.server_wifi?.ssid || serverWifiSsid || '').trim();
        let backendIp = (gatewayHostIp || '').trim();
        if (backendIp && !isValidGatewayIp(backendIp)) {
            backendIp = '';
        }
        if (!backendIp) {
            for (const candidate of [
                hw?.detected_host_ip,
                hw?.server_wifi?.detected_host_ip,
                hw?.server_wifi?.backend_ip,
                serverBackendIp
            ]) {
                const value = (candidate || '').trim();
                if (value && isValidGatewayIp(value)) {
                    backendIp = value;
                    break;
                }
            }
        }
        if (!ssid) {
            webSerialStatus = 'error';
            webSerialMsg = 'Server Wi-Fi SSID is unavailable; cannot provision the ESP32-S3.';
            return false;
        }

        const res = await provisionEspViaUsb(
            targetPort,
            ssid,
            serverWifiPassword,
            formState.backendUrl,
            backendIp
        );
        if (res.success) {
            applyProvisionedResult(res, targetPort, res.provisionedSsid || ssid);
            await refreshHardwareStatus();
            return true;
        }

        webSerialStatus = 'error';
        webSerialMsg = res.message || `Server-side USB provisioning failed on ${targetPort}.`;
        return false;
    }

    async function handlePairUsbDevice(portOverride?: string) {
        if (isWebSerialConnecting) return;

        const targetPort = portOverride || selectedUsbPort || detectedUsbPorts[0]?.port || '';
        isWebSerialConnecting = true;
        pairingPort = targetPort;
        webSerialStatus = 'idle';

        // No backend-visible node (common under Docker Desktop): skip the
        // guaranteed-to-fail server call and go straight to browser Web Serial.
        if (detectedUsbPorts.length === 0) {
            isWebSerialConnecting = false;
            await handleDirectWebSerialConnect(false);
            return;
        }

        webSerialMsg = `Reading active host Wi-Fi and provisioning ${targetPort}...`;

        try {
            const hw = await fetchDeviceStatus(formState.backendUrl);
            if (hw?.server_wifi) {
                if (hw.server_wifi.ssid) serverWifiSsid = hw.server_wifi.ssid;
                if (hw.server_wifi.backend_ip) serverBackendIp = hw.server_wifi.backend_ip;
            }

            const ssid = hw?.server_wifi?.ssid || serverWifiSsid;
            const backendIp = (gatewayHostIp || hw?.detected_host_ip || hw?.server_wifi?.detected_host_ip || hw?.server_wifi?.backend_ip || serverBackendIp || '').trim();
            const res = await provisionEspViaUsb(
                targetPort,
                ssid,
                serverWifiPassword,
                formState.backendUrl,
                backendIp
            );

            if (res.success) {
                applyProvisionedResult(res, targetPort, res.provisionedSsid || ssid || 'host Wi-Fi');
                await refreshHardwareStatus();
                return;
            }

            webSerialStatus = 'error';
            webSerialMsg = res.message || `Failed to provision ${targetPort}.`;

            if (typeof navigator !== 'undefined' && 'serial' in navigator && ssid) {
                isWebSerialConnecting = false;
                await handleDirectWebSerialConnect(false);
            }
        } catch (err: any) {
            webSerialStatus = 'error';
            webSerialMsg = err?.message || 'USB pairing failed.';
        } finally {
            isWebSerialConnecting = false;
            pairingPort = '';
        }
    }

    async function handleDirectWebSerialConnect(allowBackendFallback = true) {
        const targetPort = selectedUsbPort || detectedUsbPorts[0]?.port || '';

        if (typeof navigator === 'undefined' || !('serial' in navigator)) {
            if (allowBackendFallback) {
                await provisionViaServer(targetPort);
                return;
            }
            webSerialStatus = 'error';
            webSerialMsg = 'Web Serial API is available in Chromium, Google Chrome, or Microsoft Edge on Fedora Linux.';
            return;
        }

        isWebSerialConnecting = true;
        pairingPort = targetPort;
        webSerialStatus = 'idle';
        webSerialMsg = `Requesting serial port authorization for ${targetPort}...`;
        let port: any = null;

        try {
            let hw: any = null;
            if (!serverWifiSsid || !serverBackendIp) {
                hw = await fetchDeviceStatus(formState.backendUrl);
                if (hw?.server_wifi) {
                    if (hw.server_wifi.ssid) serverWifiSsid = hw.server_wifi.ssid;
                    if (hw.server_wifi.backend_ip) serverBackendIp = hw.server_wifi.backend_ip;
                }
            }
            if (!serverWifiSsid) {
                throw new Error('Active host Wi-Fi SSID is unavailable.');
            }

            let backendIp = (gatewayHostIp || '').trim();
            if (backendIp && !isValidGatewayIp(backendIp)) {
                webSerialStatus = 'error';
                webSerialMsg =
                    'Invalid gateway IP (Docker/loopback). Use host Wi-Fi IP, or leave blank so the ESP discovers the backend via UDP.';
                return;
            }
            if (!backendIp) {
                const candidates = [
                    hw?.detected_host_ip,
                    hw?.server_wifi?.detected_host_ip,
                    hw?.server_wifi?.backend_ip,
                    serverBackendIp,
                    typeof window !== 'undefined' ? window.location.hostname : ''
                ];
                for (const candidate of candidates) {
                    const value = (candidate || '').trim();
                    if (value && isValidGatewayIp(value)) {
                        backendIp = value;
                        break;
                    }
                }
            }

            try {
                // Request access to Espressif USB serial (or any serial port if unlisted)
                port = await withTimeout(
                    (navigator as any).serial.requestPort({
                        filters: [
                            { usbVendorId: 0x303a } // Espressif Systems
                        ]
                    }),
                    30000
                );
            } catch (authErr: any) {
                if (!allowBackendFallback) throw authErr;
                await provisionViaServer(targetPort, serverWifiSsid);
                return;
            }

            await port.open({ baudRate: 115200 });

            const encoder = new TextEncoder();
            const writer = port.writable.getWriter();
            const knownPassword = selectedNetwork && selectedNetwork.ssid === serverWifiSsid
                ? connectPassword
                : serverWifiPassword;

            // If a valid Wi-Fi SSID is provided, provision Wi-Fi credentials.
            // Otherwise, query hardware status and verify ESP32 communication.
            const hasWifiTarget = !!(serverWifiSsid && serverWifiSsid.trim().length > 0 && !serverWifiSsid.includes('Cloud-Serverless'));
            const payload = hasWifiTarget
                ? JSON.stringify({
                    cmd: 'set_wifi',
                    ssid: serverWifiSsid,
                    pass: knownPassword || '',
                    backend_ip: backendIp,
                    backend_port: 8080
                }) + '\n'
                : JSON.stringify({ cmd: 'status' }) + '\n';

            await writer.write(encoder.encode(payload));
            writer.releaseLock();

            webSerialMsg = hasWifiTarget ? 'Waiting for Wi-Fi provisioning ACK from ESP32-S3...' : 'Querying status from ESP32-S3 (/dev/ttyACM0)...';
            const ack = await readBawalAck(port, hasWifiTarget ? 30000 : 5000);

            if (ack && (ack.status === 'wifi_provisioned' || ack.status === 'ok')) {
                const ssid = ack.ssid || (hasWifiTarget ? serverWifiSsid : 'USB Direct');
                const result: EspConnectionResult = {
                    success: true,
                    status: ack.status,
                    deviceId: ack.device_id || espUsbStatus?.device_id || formState.deviceId || 'ESP32S3_USB',
                    firmwareVersion: ack.firmware_version || '1.0.0',
                    latencyMs: 4,
                    ipAddress: serverBackendIp || ack.backend_ip || 'USB Serial (/dev/ttyACM0)',
                    usbPort: targetPort || 'Web Serial (/dev/ttyACM0)',
                    provisionedSsid: ssid,
                    gatewayHostIp: gatewayHostIp || serverBackendIp || ack.backend_ip || '',
                    wifiConnected: !!ack.wifi_connected,
                    connectionMode: 'usb_serial',
                    message: hasWifiTarget ? `Provisioned ${ssid} via Web Serial` : `Connected to ${ack.device_id || 'ESP32-S3'} via Web Serial (/dev/ttyACM0)`
                };
                applyProvisionedResult(result, targetPort || '/dev/ttyACM0', ssid);
                await refreshHardwareStatus();
            } else {
                const message = ack
                    ? wifiProvisionErrorMessage(ack, serverWifiSsid)
                    : 'No BAWAL_ACK received from ESP32-S3. Check USB permissions and that idf.py monitor is closed.';
                if (allowBackendFallback) {
                    await provisionViaServer(targetPort, serverWifiSsid);
                } else {
                    webSerialStatus = 'error';
                    webSerialMsg = message;
                }
            }
        } catch (err: any) {
            if (allowBackendFallback) {
                await provisionViaServer(targetPort, serverWifiSsid);
            } else {
                webSerialStatus = 'error';
                webSerialMsg = err?.name === 'NotFoundError'
                    ? 'Serial port selection canceled.'
                    : (err?.message || 'Failed to open serial port.');
            }
        } finally {
            if (port) {
                try {
                    await port.close();
                } catch {}
            }
            isWebSerialConnecting = false;
            pairingPort = '';
        }
    }

    async function handleConnectEsp() {
        const targetId = (formState.deviceId || '').trim();
        connectionStatus = 'connecting';
        errorMessage = '';
        connectingStepText = 'Checking shared Wi-Fi network & scanning server USB ports (/dev/ttyACM*, /dev/ttyUSB*)...';

        try {
            // Detection only: let the backend prefer live LAN telemetry and
            // auto-scan USB only when no Wi-Fi device is online. Passing an
            // SSID or USB port forces USB provisioning, which fails when the
            // board is already online (or when Docker cannot see host USB).
            const res = await connectEspDevice(targetId, formState.backendUrl);

            if (res.success) {
                connectionStatus = 'connected';
                lastConnDetails = res;
                onDeviceConnectionChange?.(true, res.deviceId);
                await refreshHardwareStatus();
            } else {
                connectionStatus = 'failed';
                let msg = res.message || 'No ESP32-S3 detected on shared Wi-Fi or USB slots.';
                if (msg.toLowerCase().includes('vision pipeline') || msg.toLowerCase().includes('could not process') || msg.includes('422') || msg.includes('502') || msg.toLowerCase().includes('backend unreachable')) {
                    msg = 'No ESP32-S3 IoT node detected on cloud stream. Use Direct Browser Connect (/dev/ttyACM0) or connect to your local backend (http://localhost:8080).';
                }
                errorMessage = msg;
                onDeviceConnectionChange?.(false, targetId);
                await refreshHardwareStatus();
            }
        } catch (err: any) {
            connectionStatus = 'failed';
            let msg = err?.message || 'Failed to probe ESP32-S3 hardware.';
            if (msg.toLowerCase().includes('vision pipeline') || msg.toLowerCase().includes('could not process') || msg.includes('422') || msg.includes('502') || msg.toLowerCase().includes('backend unreachable')) {
                msg = 'No ESP32-S3 IoT node detected on cloud stream. Use Direct Browser Connect (/dev/ttyACM0) or connect to your local backend (http://localhost:8080).';
            }
            errorMessage = msg;
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
        pairingGraceUntil = 0;
        errorMessage = '';
        onDeviceConnectionChange?.(false, '');
    }

    let isSaved = $state(false);

    function handleSubmit(e: SubmitEvent) {
        e.preventDefault();
        formState.backendUrl = setConfiguredApiBase(formState.backendUrl);
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
                        {:else if connectionStatus === 'wifi_only'}
                            Wi-Fi OK (Backend Offline)
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

            <!-- Local live backend target for MJPEG + SIMD inference -->
            <div class="fedora-group">
                <div class="fedora-group-header">
                    <div class="header-title-box">
                        <span class="section-icon">🖥️</span>
                        <div>
                            <h4>Local Live Backend (MJPEG + SIMD)</h4>
                            <p class="fedora-subtext">ESP32 camera and vision engine stay on the host; cloud/Lambda handles reports only</p>
                        </div>
                    </div>
                    <button
                        type="button"
                        class="fedora-pill-btn"
                        onclick={handleDetectLocalBackend}
                        disabled={detectingLocalBackend}
                    >
                        {#if detectingLocalBackend}
                            <span class="btn-spinner"></span> Detecting...
                        {:else}
                            Detect Local Backend
                        {/if}
                    </button>
                </div>
                <div class="local-backend-row">
                    <input
                        class="fedora-input"
                        bind:value={formState.backendUrl}
                        placeholder="http://192.168.100.165:8080 or http://localhost:8080"
                        spellcheck="false"
                        autocapitalize="off"
                        autocorrect="off"
                    />
                    <button type="button" class="fedora-accent-pill-btn" onclick={applyLiveBackendUrl}>
                        Apply &amp; Detect
                    </button>
                </div>
                <p class="port-detail-text">
                    When <code>ACM0</code> is detected, the gateway IP auto-fills from this backend.
                    For phone access use the laptop LAN IP (or open the local frontend at
                    <code>http://&lt;laptop-ip&gt;:3001</code>); HTTPS cloud pages may block HTTP MJPEG.
                </p>
                {#if localBackendMessage}
                    <div class="fedora-notice idle"><span>{localBackendMessage}</span></div>
                {/if}
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
                            onclick={() => handleDirectWebSerialConnect(true)}
                            disabled={isWebSerialConnecting}
                            title="Pair ESP32-S3 via Web Serial, falling back to server-side USB provisioning"
                        >
                            {#if isWebSerialConnecting}
                                <span class="btn-spinner"></span> Pairing...
                            {:else}
                                <span class="lightning-icon">⚡</span> Direct Browser Connect
                            {/if}
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
                                        {:else if espUsbStatus && espUsbStatus.port === portInfo.port}
                                            <div class="port-ssid-indicator">
                                                <span class="port-subdetail-muted">Wi-Fi: press Probe to read the board's current network</span>
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
                                        class="fedora-accent-pill-btn"
                                        onclick={() => handlePairUsbDevice(portInfo.port)}
                                        disabled={isWebSerialConnecting}
                                        title="Auto-provision the active host Wi-Fi to this ESP32-S3"
                                    >
                                        {#if pairingPort === portInfo.port}
                                            <span class="btn-spinner"></span> Pairing...
                                        {:else}
                                            ⚡ Pair
                                        {/if}
                                    </button>
                                    <button
                                        type="button"
                                        class="fedora-row-action-btn"
                                        onclick={() => handleProbeUsbPort(portInfo.port)}
                                        disabled={isProbingUsb || isWebSerialConnecting}
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
                    {:else if !backendOnline}
                        <div class="fedora-list-row empty-row">
                            <div class="row-leading">
                                <span class="empty-icon">⚠️</span>
                                <div>
                                    <span class="empty-title">Backend offline</span>
                                    <p class="port-detail-text">Cannot reach the backend API. Start it on the host with <code>just run-backend</code> (Docker Desktop cannot expose host USB or nmcli), then press <strong>Rescan Ports</strong>.</p>
                                </div>
                            </div>
                        </div>
                    {:else}
                        <div class="fedora-list-row empty-row">
                            <div class="row-leading">
                                <span class="empty-icon">ℹ️</span>
                                <div>
                                    <span class="empty-title">No USB serial ports detected</span>
                                    <p class="port-detail-text">Plug an ESP32-S3 into the backend host and press <strong>Rescan Ports</strong>, or use <strong>Direct Browser Connect</strong> / Web Serial below.</p>
                                </div>
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
                                    {#if !backendOnline}
                                        <span class="empty-title">Backend offline</span>
                                        <p class="port-detail-text">Start the host backend with <code>just run-backend</code> to scan networks through <code>nmcli</code>, then press <strong>Scan Now</strong>.</p>
                                    {:else if isScanningWifi}
                                        <span class="empty-title">Scanning for available Wi-Fi networks...</span>
                                        <p class="port-detail-text">Querying the backend via <code>nmcli</code>.</p>
                                    {:else}
                                        <span class="empty-title">No Wi-Fi networks found</span>
                                        <p class="port-detail-text">Press <strong>Scan Now</strong> and verify NetworkManager (<code>nmcli</code>) is running on the backend host.</p>
                                    {/if}
                                </div>
                            </div>
                            <div class="row-trailing" style="display: flex; gap: 8px;">
                                <button type="button" class="fedora-pill-btn" onclick={handleScanWifi}>
                                    Scan Now
                                </button>
                                <button type="button" class="fedora-pill-btn primary" onclick={handleOpenManualWifiModal}>
                                    ➕ Enter Wi-Fi Manually
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
                                        Connect ESP32-S3 node to local server Wi-Fi <strong>{selectedNetwork.ssid}</strong> (Gateway: {serverBackendIp || 'auto-detected'})
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
                                <input id="wifi-dialog-ssid" type="text" class="fedora-input" bind:value={selectedSsid} placeholder="Wi-Fi SSID" />
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
                                        {#each portOptions as port}
                                            <option value={port}>{portLabel(port)}</option>
                                        {/each}
                                        {#if isWebSerialAvailable()}
                                            <option value="web_serial">⚡ Direct Browser Connect (Web Serial - /dev/ttyACM0)</option>
                                        {/if}
                                        {#if portOptions.length === 0 && !isWebSerialAvailable()}
                                            <option value="">No USB serial ports detected</option>
                                        {/if}
                                    </select>
                                    {#if portOptions.length === 0 && !isWebSerialAvailable()}
                                        <p class="port-detail-text" style="color: #ea580c; font-size: 0.75rem; margin-top: 4px;">
                                            ⚠️ Web Serial is blocked or unsupported in this browser (e.g. Brave Shields). Use Chrome/Edge or enable Serial in brave://settings/content/serialPorts to pair /dev/ttyACM0 directly.
                                        </p>
                                    {/if}
                                </div>
                                <div class="dialog-field half">
                                    <label for="wifi-dialog-backend">Gateway Host IP</label>
                                    <input
                                        id="wifi-dialog-backend"
                                        type="text"
                                        class="fedora-input"
                                        autocomplete="off"
                                        autocorrect="off"
                                        autocapitalize="off"
                                        spellcheck="false"
                                        bind:value={gatewayHostIp}
                                        oninput={() => (gatewayHostIpEdited = true)}
                                        placeholder={detectedHostIp || 'Auto-detect laptop Wi-Fi IP'}
                                    />
                                    {#if gatewayHostIp && !isValidGatewayIp(gatewayHostIp)}
                                        <span class="gateway-ip-error">Docker/loopback IP is not allowed. Use host Wi-Fi IP or leave blank.</span>
                                    {/if}
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
                {:else if connectionStatus === 'wifi_only'}
                    <div class="esp-failed-box">
                        <div class="failed-info">
                            <span class="failed-icon">📶</span>
                            <span class="failed-text">{lastConnDetails.message || 'ESP32-S3 is on Wi-Fi but the backend cannot reach it.'}</span>
                        </div>
                        <button type="button" class="btn-retry-conn" onclick={handleConnectEsp}>
                            ↻ Retry
                        </button>
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
                        <span class="idle-text">Select your Wi-Fi network above to provision ESP32-S3, or plug into a detected USB serial port to automatically apply credentials.</span>
                    </div>
                {/if}
            </div>
        </div>

        <!-- 2. Inspection Source (Live Camera vs Mock Video) -->
        <div class="settings-card">
            <div class="section-header">
                <span class="section-icon">🎬</span>
                <h3>Inspection Source</h3>
            </div>
            <div class="source-mode-grid">
                <button
                    type="button"
                    class="source-mode-btn"
                    class:active={inspectionSource === 'live'}
                    disabled={sourceModeBusy}
                    onclick={() => applyInspectionSource('live')}
                >
                    <strong>Live ESP32 Camera</strong>
                    <small>Stream frames from the physical sensor node in real time.</small>
                </button>
                <button
                    type="button"
                    class="source-mode-btn"
                    class:active={inspectionSource === 'mock'}
                    disabled={sourceModeBusy}
                    onclick={() => applyInspectionSource('mock')}
                >
                    <strong>Mock MP4 Video</strong>
                    <small>Replay a bundled sample video through the same vision pipeline.</small>
                </button>
            </div>
        </div>

        <!-- 3. Detection Confidence Thresholds -->
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

        <!-- 4. Engine Hardware Accelerators -->
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

    {#if connectToast}
        <div class="connect-toast">{connectToast}</div>
    {/if}
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

    .connect-toast {
        position: fixed;
        right: 24px;
        bottom: 24px;
        z-index: 3000;
        max-width: 420px;
        background: var(--accent-green-dim);
        color: var(--accent-green);
        border: 1px solid rgba(34, 197, 94, 0.35);
        padding: 12px 18px;
        border-radius: var(--radius-lg);
        font-size: 0.85rem;
        font-weight: 700;
        box-shadow: var(--shadow-card);
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

    .source-mode-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 10px;
    }

    .source-mode-btn {
        display: flex;
        flex-direction: column;
        align-items: flex-start;
        gap: 4px;
        padding: 12px 14px;
        text-align: left;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        cursor: pointer;
        transition: border-color 0.15s ease, background 0.15s ease;
    }

    .source-mode-btn:hover:not(:disabled) {
        border-color: var(--accent-blue);
    }

    .source-mode-btn.active {
        border-color: var(--accent-blue);
        background: color-mix(in srgb, var(--accent-blue) 10%, var(--bg-inset));
    }

    .source-mode-btn:disabled {
        opacity: 0.65;
        cursor: wait;
    }

    .source-mode-btn strong {
        font-size: 0.8rem;
        color: var(--text-primary);
    }

    .source-mode-btn small {
        font-size: 0.7rem;
        line-height: 1.4;
        color: var(--text-muted);
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

    .esp-status-chip.wifi_only {
        color: var(--accent-amber);
        background: var(--accent-amber-dim);
        border-color: rgba(217, 119, 6, 0.35);
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

    .status-dot.wifi_only {
        background: var(--accent-amber);
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

    .esp-connection-card.wifi_only {
        border-color: rgba(217, 119, 6, 0.3);
        background: rgba(217, 119, 6, 0.03);
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

    .local-backend-row {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .local-backend-row .fedora-input {
        flex: 1;
        min-width: 0;
    }

    .local-backend-row .fedora-accent-pill-btn {
        flex-shrink: 0;
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

    .gateway-ip-error {
        display: block;
        margin-top: 4px;
        color: #fca5a5;
        font-size: 0.72rem;
        line-height: 1.3;
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

