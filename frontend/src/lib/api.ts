import type { InspectionRecord, TelemetryPayload, FoodDetection, InferenceLogEntry } from './types';

const BASE_URL = '/api/v1';

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
                humidity: data.humidity || 0,
                gas_voc: data.gas_voc || 0,
                pressure: data.pressure || 0,
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

// Generate realistic mock telemetry and food detection for offline demo & development
export function createMockInspectionFrame(): {
    detections: FoodDetection[];
    telemetry: TelemetryPayload;
    log: InferenceLogEntry;
} {
    // 4 standard balanced MBG meal slots
    const standardMeal = [
        { name: 'nasi', weight: 160, cat: 'Carbohydrate' as const, baseSim: 0.94 },
        { name: 'ayam', weight: 115, cat: 'Protein' as const, baseSim: 0.91 },
        { name: 'kangkung', weight: 80, cat: 'Vegetable' as const, baseSim: 0.88 },
        { name: 'pisang', weight: 110, cat: 'Fruit' as const, baseSim: 0.93 }
    ];

    const detections: FoodDetection[] = standardMeal.map((item) => ({
        food_name: item.name,
        similarity_rate: Number(Math.max(0.70, item.baseSim - (Math.random() * 0.05)).toFixed(2)),
        weight_grams: item.weight + Math.floor((Math.random() * 8) - 4),
        category: item.cat
    }));

    const temp = Number((24.5 + (Math.random() * 1.5)).toFixed(1));
    const hum = Number((62.0 + (Math.random() * 4.0)).toFixed(1));
    const gas = Number((105.0 + (Math.random() * 12.0)).toFixed(1));
    const press = Number((1013.25 + (Math.random() * 0.5)).toFixed(2));

    const telemetry: TelemetryPayload = {
        device_id: 'ESP32S3_441BF6C95678',
        firmware_version: '1.0.4-prod',
        device_core: 2,
        temperature: temp,
        humidity: hum,
        gas_voc: gas,
        pressure: press,
        cpu_load: Math.floor(12 + Math.random() * 10),
        timestamp: Math.floor(Date.now() / 1000)
    };

    const latency = Number((4.8 + Math.random() * 2.2).toFixed(1));
    const foodsList = detections.map(d => `${d.food_name}: ${Math.round(d.similarity_rate * 100)}%`).join(', ');

    const log: InferenceLogEntry = {
        id: Math.random().toString(36).substring(2, 9),
        timestamp: new Date().toLocaleTimeString(),
        level: 'AI',
        message: `Processed tray frame in ${latency} ms | Detections: [${foodsList}] | VOC: ${gas} kΩ`,
        latencyMs: latency
    };

    return { detections, telemetry, log };
}
