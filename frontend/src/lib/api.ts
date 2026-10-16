import type { InspectionRecord, TelemetryPayload, FoodDetection, InferenceLogEntry, EngineStatus, EspConnectionResult, DeviceHardwareStatus } from './types';

const BASE_URL = '/api/v1';

export async function fetchEngineStatus(baseUrl = BASE_URL): Promise<EngineStatus> {
    try {
        const response = await fetch(`${baseUrl}/system/engines`, {
            headers: { 'Accept': 'application/json' }
        });
        if (response.ok) {
            return await response.json();
        }
    } catch {
        // offline or fallback
    }
    return {
        arch: 'unknown',
        graviton_available: false,
        local_engine_available: true
    };
}

export async function fetchRecords(baseUrl = BASE_URL): Promise<InspectionRecord[]> {
    try {
        const response = await fetch(`${baseUrl}/records`, {
            headers: { 'Accept': 'application/json' }
        });
        if (!response.ok) {
            throw new Error(`HTTP error ${response.status}`);
        }
        return await response.json();
    } catch (err) {
        console.warn('[API] Could not fetch records from backend:', err);
        return [];
    }
}

export async function fetchCameraStatus(baseUrl = BASE_URL): Promise<{ active: boolean; deviceId: string; fps: number }> {
    try {
        const response = await fetch(`${baseUrl}/camera/active`);
        if (response.ok) {
            const data = await response.json();
            if (data.device_id) {
                return { active: true, deviceId: data.device_id, fps: Number(data.fps) || 0.0 };
            }
        }
    } catch {
        // network or offline error
    }
    return { active: false, deviceId: '', fps: 0.0 };
}

export async function fetchLatestTelemetry(baseUrl = BASE_URL): Promise<TelemetryPayload | null> {
    try {
        const response = await fetch(`${baseUrl}/telemetry/latest`, {
            headers: { 'Accept': 'application/json' }
        });
        if (response.ok) {
            const data = await response.json();
            const isConnected = data.is_connected !== undefined
                ? Boolean(data.is_connected)
                : (data.cpu_temperature != null && data.cpu_load != null);
            return {
                device_id: data.device_id || '',
                firmware_version: data.firmware_version || '1.0.0',
                device_core: data.device_core || 2,
                cpu_temperature: isConnected && data.cpu_temperature != null ? Number(data.cpu_temperature) : null,
                temperature: isConnected && data.temperature != null ? Number(data.temperature) : null,
                cpu_load: isConnected && data.cpu_load != null ? Number(data.cpu_load) : null,
                heap_free: isConnected && (data.heap_free != null || data.free_heap != null) ? Number(data.heap_free ?? data.free_heap) : null,
                free_heap: isConnected && (data.heap_free != null || data.free_heap != null) ? Number(data.heap_free ?? data.free_heap) : null,
                rssi: isConnected && data.rssi != null ? Number(data.rssi) : null,
                timestamp: data.timestamp
                    ? Number(data.timestamp)
                    : (data.created_at ? Math.floor(new Date(data.created_at).getTime() / 1000) : Math.floor(Date.now() / 1000)),
                is_connected: isConnected
            };
        }
    } catch {
        // offline or no telemetry
    }
    return null;
}

export async function fetchLatestInspection(baseUrl = BASE_URL): Promise<InspectionRecord | null> {
    try {
        const response = await fetch(`${baseUrl}/inspections/latest`, {
            headers: { 'Accept': 'application/json' }
        });
        if (response.ok) {
            return await response.json();
        }
    } catch {
        // offline or no inspections
    }
    return null;
}

export async function triggerManualInspection(
    deviceId: string,
    detections: FoodDetection[],
    baseUrl = BASE_URL
): Promise<{ status: string; id?: string }> {
    try {
        const response = await fetch(`${baseUrl}/inspections`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Accept': 'application/json'
            },
            body: JSON.stringify({
                device_id: deviceId || 'ESP32S3_441BF6C95678',
                detections: detections
            })
        });
        if (response.ok) {
            return await response.json();
        }
    } catch (err) {
        console.error('[API] Failed to submit manual inspection:', err);
    }
    return { status: 'error' };
}

export async function fetchDeviceStatus(baseUrl = BASE_URL): Promise<DeviceHardwareStatus | null> {
    try {
        const response = await fetch(`${baseUrl}/devices/status`, {
            headers: { 'Accept': 'application/json' }
        });
        if (response.ok) {
            return await response.json();
        }
    } catch {
        // backend unreachable
    }
    return null;
}

export async function connectEspDevice(
    deviceId: string,
    baseUrl = BASE_URL,
    options?: { usbPort?: string; ssid?: string; password?: string }
): Promise<EspConnectionResult> {
    const cleanId = (deviceId || '').trim();

    try {
        const response = await fetch(`${baseUrl}/devices/connect`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Accept': 'application/json'
            },
            body: JSON.stringify({
                device_id: cleanId,
                usb_port: options?.usbPort || '',
                ssid: options?.ssid || '',
                password: options?.password || ''
            })
        });
        const data = await response.json().catch(() => ({}));
        if (response.ok && data.success) {
            return {
                success: true,
                deviceId: data.device_id || cleanId,
                firmwareVersion: data.firmware_version || '1.0.0',
                latencyMs: data.latency_ms ?? 8,
                ipAddress: data.ip_address || '',
                usbPort: data.usb_port || '',
                provisionedSsid: data.provisioned_ssid || '',
                connectionMode: data.connection_mode || 'wifi_lan',
                message: data.message || 'Connected to ESP32-S3 hardware'
            };
        }
        return {
            success: false,
            deviceId: cleanId,
            connectionMode: 'disconnected',
            message: data.message || data.error || 'No ESP32-S3 detected on the server Wi-Fi network or USB serial slots (/dev/ttyACM*, /dev/ttyUSB*).'
        };
    } catch (err: any) {
        return {
            success: false,
            deviceId: cleanId,
            connectionMode: 'disconnected',
            message: err?.message || 'Unable to reach backend server to probe ESP32-S3 hardware.'
        };
    }
}

export async function provisionEspViaUsb(
    usbPort: string,
    ssid: string,
    password: string,
    baseUrl = BASE_URL
): Promise<EspConnectionResult> {
    try {
        const response = await fetch(`${baseUrl}/devices/usb/provision`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Accept': 'application/json'
            },
            body: JSON.stringify({
                usb_port: usbPort,
                ssid,
                password
            })
        });
        const data = await response.json().catch(() => ({}));
        if (response.ok && data.success) {
            return {
                success: true,
                deviceId: data.device_id || 'ESP32S3_USB',
                firmwareVersion: data.firmware_version || '1.0.0',
                ipAddress: data.usb_port || usbPort,
                usbPort: data.usb_port || usbPort,
                provisionedSsid: data.provisioned_ssid || ssid,
                connectionMode: 'usb_serial',
                message: data.message || `Provisioned server Wi-Fi '${ssid}' over ${usbPort}`
            };
        }
        return {
            success: false,
            deviceId: '',
            connectionMode: 'disconnected',
            message: data.message || 'USB Wi-Fi provisioning failed.'
        };
    } catch (err: any) {
        return {
            success: false,
            deviceId: '',
            connectionMode: 'disconnected',
            message: err?.message || 'Network error while provisioning ESP32-S3 over USB.'
        };
    }
}

export async function disconnectEspDevice(
    deviceId: string,
    baseUrl = BASE_URL
): Promise<boolean> {
    try {
        const response = await fetch(`${baseUrl}/devices/disconnect`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Accept': 'application/json'
            },
            body: JSON.stringify({ device_id: deviceId })
        });
        return response.ok;
    } catch {
        return true;
    }
}

export async function fetchWiFiNetworks(baseUrl = BASE_URL): Promise<WiFiNetworkInfo[]> {
    try {
        const response = await fetch(`${baseUrl}/devices/wifi/networks`, {
            headers: { 'Accept': 'application/json' }
        });
        if (response.ok) {
            const data = await response.json();
            return data.networks || [];
        }
    } catch {
        // fallback
    }
    return [];
}

export async function probeEspUsbPort(port?: string, baseUrl = BASE_URL): Promise<{
    success: boolean;
    port?: string;
    deviceId?: string;
    currentSsid?: string;
    wifiConnected?: boolean;
    temperature?: number;
    cpuLoad?: number;
    message?: string;
}> {
    try {
        const response = await fetch(`${baseUrl}/devices/usb/probe`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Accept': 'application/json'
            },
            body: JSON.stringify({ port })
        });
        const data = await response.json().catch(() => ({}));
        return {
            success: response.ok && data.success,
            port: data.port,
            deviceId: data.device_id,
            currentSsid: data.current_ssid,
            wifiConnected: data.wifi_connected,
            temperature: data.temperature,
            cpuLoad: data.cpu_load,
            message: data.message
        };
    } catch (err: any) {
        return {
            success: false,
            message: err?.message || 'USB probe request failed.'
        };
    }
}


