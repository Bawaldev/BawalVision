import type { InspectionRecord, TelemetryPayload, FoodDetection, InferenceLogEntry, EngineStatus, EspConnectionResult, DeviceHardwareStatus, WiFiNetworkInfo } from './types';

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

/**
 * @brief Submits a manual food inspection request to the backend.
 * @param deviceId Target camera or mock simulation device identifier.
 * @param detections Array of detected food bounding boxes and classification scores.
 * @param baseUrl Base HTTP endpoint URL for the backend API.
 * @param sourceMode Stream source mode ('live' or 'mock').
 * @param videoSource Relative video file path if in mock simulation mode.
 * @param videoUUID Unique deterministic UUID of the mock video.
 * @return Promise resolving to status and created inspection ID.
 */
export async function triggerManualInspection(
    deviceId: string,
    detections: FoodDetection[],
    baseUrl = BASE_URL,
    sourceMode?: string,
    videoSource?: string,
    videoUUID?: string
): Promise<{ status: string; id?: string }> {
    try {
        const isMock = sourceMode === 'mock' || deviceId === 'MOCK_VIDEO_CAM' || (deviceId && deviceId.startsWith('MOCK_'));
        const endpoint = isMock ? `${baseUrl}/inspections/mock` : `${baseUrl}/inspections`;
        const headers: Record<string, string> = {
            'Content-Type': 'application/json',
            'Accept': 'application/json'
        };
        if (isMock) {
            headers['X-Stream-Source'] = 'MOCK_VIDEO';
            headers['X-Inspection-Source'] = 'MOCK_VIDEO';
            headers['X-Inspection-Struct'] = 'MockInspectionRequest';
            if (videoSource) headers['X-Mock-Video-Source'] = videoSource;
            if (videoUUID) headers['X-Mock-Video-UUID'] = videoUUID;
        }

        const bodyPayload = isMock ? {
            mock_id: videoUUID || 'MOCK_VIDEO_CAM',
            device_id: 'MOCK_VIDEO_CAM',
            source_type: 'mock_video',
            inspection_struct: 'MockInspectionRequest',
            video_source: videoSource || '',
            video_uuid: videoUUID || '',
            detections: detections
        } : {
            device_id: deviceId || 'ESP32S3_441BF6C95678',
            detections: detections
        };

        const response = await fetch(endpoint, {
            method: 'POST',
            headers,
            body: JSON.stringify(bodyPayload)
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
                status: data.status,
                deviceId: data.device_id || cleanId,
                firmwareVersion: data.firmware_version || '1.0.0',
                latencyMs: data.latency_ms ?? 8,
                ipAddress: data.ip_address || '',
                usbPort: data.usb_port || '',
                provisionedSsid: data.provisioned_ssid || '',
                wifiConnected: data.wifi_connected,
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
    baseUrl = BASE_URL,
    gatewayHostIp = ''
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
                password,
                backend_ip: gatewayHostIp,
                gateway_host_ip: gatewayHostIp
            })
        });
        const data = await response.json().catch(() => ({}));
        if (response.ok && data.success) {
            return {
                success: true,
                status: data.status,
                deviceId: data.device_id || 'ESP32S3_USB',
                firmwareVersion: data.firmware_version || '1.0.0',
                ipAddress: data.usb_port || usbPort,
                usbPort: data.usb_port || usbPort,
                provisionedSsid: data.provisioned_ssid || ssid,
                gatewayHostIp: data.gateway_host_ip || data.backend_ip || gatewayHostIp,
                wifiConnected: !!data.wifi_connected,
                connectionMode: 'usb_serial',
                message: data.message || `Provisioned Wi-Fi '${ssid}' over ${usbPort}`
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

export async function fetchConfidenceThreshold(baseUrl = BASE_URL): Promise<{ threshold: number; min: number; max: number; default: number }> {
    try {
        const response = await fetch(`${baseUrl}/system/threshold`, {
            headers: { 'Accept': 'application/json' }
        });
        if (response.ok) {
            return await response.json();
        }
    } catch {
        // fallback
    }
    return { threshold: 0.60, min: 0.45, max: 0.98, default: 0.60 };
}

export async function updateConfidenceThreshold(threshold: number, baseUrl = BASE_URL): Promise<boolean> {
    try {
        const response = await fetch(`${baseUrl}/system/threshold`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Accept': 'application/json'
            },
            body: JSON.stringify({ threshold })
        });
        return response.ok;
    } catch {
        return false;
    }
}



