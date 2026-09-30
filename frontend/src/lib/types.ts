export interface FoodDetection {
    food_name: string;
    similarity_rate: number;
    weight_grams?: number;
    box?: [number, number, number, number]; // [x, y, width, height]
    category?: 'Carbohydrate' | 'Protein' | 'Vegetable' | 'Fruit' | 'Side';
}

export interface FoodCompartmentItem {
    id: number;
    name: string;
    detectedFood: string;
    similarityRate: number;
    weightGrams: number;
    spoilageIndex: number;
    status: 'PASS' | 'REVIEW' | 'REJECT';
}

export interface InspectionRecord {
    id: string;
    user_id?: string;
    device_id: string;
    firmware_version: string;
    created_at: string;
    image_s3_uri?: string;
    temperature: number;
    humidity: number;
    gas_voc: number;
    pressure: number;
    cpu_load: number;
    is_spoiled: boolean;
    t_safe_hours: number;
    inspection_foods?: Array<{
        id: string;
        inspection_id: string;
        food_item_id: number;
        weight_grams: number;
        food_item?: {
            id: number;
            name: string;
        };
    }>;
}

export interface TelemetryPayload {
    device_id: string;
    device_name?: string;
    firmware_version: string;
    device_core: number;
    temperature: number;
    humidity: number;
    gas_voc: number;
    pressure: number;
    cpu_load: number;
    timestamp: number;
}

export interface InferenceLogEntry {
    id: string;
    timestamp: string;
    level: 'INFO' | 'WARN' | 'ERROR' | 'SUCCESS' | 'AI';
    message: string;
    latencyMs?: number;
}

export interface DashboardStats {
    totalInspections: number;
    totalAccepted: number;
    totalRejected: number;
    passRate: number;
    averageLatencyMs: number;
    activeDevice: string;
    engineType: 'AWS_GRAVITON3' | 'LOCAL_JETSON';
    cloudSyncOnline: boolean;
    bufferedRecords: number;
}

export interface EngineStatus {
    arch: string;
    graviton_available: boolean;
    local_engine_available: boolean;
}

export interface SystemConfig {
    backendUrl: string;
    deviceId: string;
    streamIntervalMs: number;
    enableMockIfOffline: boolean;
    minConfidenceThreshold: number;
    vocWarningThreshold: number;
    vocDangerThreshold: number;
    gravitonEngineEnabled: boolean;
    gravitonEngineAvailable?: boolean;
    localEngineEnabled: boolean;
    localEngineAvailable?: boolean;
}
