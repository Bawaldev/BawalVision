import type { InspectionRecord, TelemetryPayload, FoodDetection, InferenceLogEntry, EngineStatus } from './types';

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

export async function fetchCameraStatus(baseUrl = BASE_URL): Promise<{ active: boolean; deviceId: string }> {
    try {
        const response = await fetch(`${baseUrl}/camera/active`);
        if (response.ok) {
            const data = await response.json();
            if (data.device_id) {
                return { active: true, deviceId: data.device_id };
            }
        }
    } catch {
        // network or offline error
    }
    return { active: false, deviceId: '' };
}

export async function fetchLatestTelemetry(baseUrl = BASE_URL): Promise<TelemetryPayload | null> {
    try {
        const response = await fetch(`${baseUrl}/telemetry/latest`, {
            headers: { 'Accept': 'application/json' }
        });
        if (response.ok) {
            const data = await response.json();
            return {
                device_id: data.device_id || '',
                firmware_version: data.firmware_version || '1.0.0',
                device_core: data.device_core || 2,
                temperature: data.temperature || 0,
                cpu_load: data.cpu_load || 0,
                timestamp: data.created_at ? Math.floor(new Date(data.created_at).getTime() / 1000) : Math.floor(Date.now() / 1000)
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
