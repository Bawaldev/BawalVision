export interface FoodDetection {
    food_name: string;
    similarity_rate: number;
    spoilage_score?: number;
    status?: 'PASS' | 'REVIEW' | 'REJECT';
    box?: [number, number, number, number]; // [x, y, width, height]
    category?: 'Carbohydrate' | 'Protein' | 'Vegetable' | 'Fruit' | 'Side';
}

export interface SceneOpticalMetrics {
    avg_r: number;
    avg_g: number;
    avg_b: number;
    lux_luminance: number;
    color_temperature: number;
    white_balance_status: 'BALANCED' | 'WARM' | 'COOL' | 'UNDEREXPOSED' | 'OVEREXPOSED' | string;
}

export interface InspectionFoodEntry {
    id: string;
    inspection_id: string;
    food_item_id: number;
    similarity_rate: number;
    spoilage_score: number;
    status: 'PASS' | 'REVIEW' | 'REJECT';
    food_item?: {
        id: number;
        name: string;
    };
}

export interface InspectionRecord {
    id: string;
    mock_id?: string;
    video_uuid?: string;
    device_id: string;
    source_type?: 'live_esp32' | 'mock_video' | string;
    inspection_struct?: 'InspectionRecord' | 'MockInspectionRecord' | string;
    video_source?: string;
    engine_pipeline?: string;
    firmware_version?: string;
    created_at: string;
    image_s3_uri?: string;
    cpu_temperature?: number;
    temperature?: number;
    cpu_load?: number;
    is_spoiled: boolean;
    t_safe_hours: number;
    lux_luminance?: number;
    white_balance_status?: string;
    color_temperature?: number;
    scene_optical?: SceneOpticalMetrics;
    inspection_foods?: InspectionFoodEntry[];
}

export interface MockInspectionRecord {
    id: string;
    mock_id: string;
    video_uuid: string;
    device_id: string;
    source_type: 'mock_video';
    inspection_struct: 'MockInspectionRecord';
    video_source?: string;
    engine_pipeline: string;
    created_at: string;
    is_spoiled: boolean;
    t_safe_hours: number;
    lux_luminance?: number;
    white_balance_status?: string;
    color_temperature?: number;
    scene_optical?: SceneOpticalMetrics;
    detections?: FoodDetection[];
    inspection_foods?: InspectionFoodEntry[];
}

export interface TelemetryPayload {
    device_id: string;
    mock_id?: string;
    video_uuid?: string;
    source_type?: 'live_esp32' | 'mock_video' | string;
    telemetry_struct?: 'TelemetryIngressPayload' | 'MockTelemetryPayload' | string;
    device_name?: string;
    firmware_version: string;
    device_core: number;
    cpu_temperature?: number | null;
    temperature: number | null;
    cpu_load?: number | null;
    cpu_core0_load?: number | null;
    cpu_core1_load?: number | null;
    heap_free?: number | null;
    free_heap?: number | null;
    rssi?: number | null;
    server_memory_mb?: number | null;
    camera_model?: string | null;
    video_source?: string | null;
    engine_pipeline?: string | null;
    is_mock_active?: boolean;
    timestamp: number;
    is_connected?: boolean;
    scene_optical?: SceneOpticalMetrics;
}

export interface MockTelemetryPayload {
    mock_id: string;
    video_uuid: string;
    device_id: string;
    source_type: 'mock_video';
    telemetry_struct: 'MockTelemetryPayload';
    video_source?: string;
    frame_id: number;
    timestamp: number;
    is_rewind: boolean;
    connection_mode: 'mock_simulation';
    engine_pipeline: string;
    scene_optical?: SceneOpticalMetrics;
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
    num_cpu?: number;
    parallel_cores?: number;
    server_mem_alloc_mb?: number;
    server_mem_sys_mb?: number;
    host_mem_used_mb?: number;
    host_mem_total_mb?: number;
    memory_usage_mb?: number;
}

export interface SystemConfig {
    backendUrl: string;
    deviceId: string;
    streamIntervalMs: number;
    enableMockIfOffline: boolean;
    minConfidenceThreshold: number;
    gravitonEngineEnabled: boolean;
    gravitonEngineAvailable?: boolean;
    localEngineEnabled: boolean;
    localEngineAvailable?: boolean;
}

export interface USBPortInfo {
    port: string;
    name: string;
    driver?: string;
    vendor_id?: string;
    product_id?: string;
    manufacturer?: string;
    product?: string;
    is_esp32: boolean;
    device_id?: string;
    current_ssid?: string;
    wifi_connected?: boolean;
    temperature?: number;
}

export interface WiFiNetworkInfo {
    ssid: string;
    signal: number;
    security: string;
    is_server?: boolean;
    is_saved?: boolean;
    bssid?: string;
}

export interface DeviceHardwareStatus {
    connected: boolean;
    connection_mode: 'wifi_lan' | 'usb_serial' | 'disconnected';
    device_id: string;
    firmware_version: string;
    ip_address: string;
    usb_ports: USBPortInfo[];
    esp_usb_status?: {
        port: string;
        device_id: string;
        current_ssid: string;
        wifi_connected: boolean;
        temperature: number;
    };
    wifi_networks?: WiFiNetworkInfo[];
    server_wifi: {
        ssid: string;
        has_password: boolean;
        backend_ip: string;
        backend_port: string;
    };
}

export interface EspConnectionResult {
    success: boolean;
    status?: string;
    deviceId: string;
    firmwareVersion?: string;
    rssi?: number;
    latencyMs?: number;
    ipAddress?: string;
    usbPort?: string;
    provisionedSsid?: string;
    wifiConnected?: boolean;
    connectionMode?: 'wifi_lan' | 'usb_serial' | 'disconnected';
    message?: string;
}

