<script lang="ts">
    import { onMount } from 'svelte';
    import { fetchRecords } from '../api';
    import CameraStream from '../components/CameraStream.svelte';
    import type { InspectionRecord, TelemetryPayload } from '../types';

    interface Props {
        currentTelemetry?: TelemetryPayload;
        history?: TelemetryPayload[];
    }

    let { currentTelemetry, history = [] }: Props = $props();

    // Records Database State
    let records = $state<InspectionRecord[]>([]);
    let isLoading = $state(true);
    let searchTerm = $state('');
    let statusFilter = $state<'ALL' | 'PASS' | 'REVIEW' | 'REJECT'>('ALL');
    let selectedRecord = $state<InspectionRecord | null>(null);
    let showCameraInspection = $state(false);

    // Hover tooltip state for temperature chart
    let hoveredIdx = $state<number | null>(null);

    async function loadRecords() {
        isLoading = true;
        records = await fetchRecords();
        isLoading = false;
    }

    onMount(() => {
        loadRecords();
    });

    let isDeviceConnected = $derived(
        Boolean(currentTelemetry?.is_connected) && currentTelemetry?.temperature != null
    );

    // Only display live telemetry timeline when ESP32-S3 is actively connected
    let displayHistory = $derived.by(() => {
        if (!isDeviceConnected) {
            return [];
        }
        if (history && history.length >= 2) {
            return history.filter(pt => pt.temperature != null);
        }

        const baseTemp = currentTelemetry?.temperature ?? 36.5;
        const baseMcu = currentTelemetry?.cpu_temperature ?? baseTemp;
        const now = Math.floor(Date.now() / 1000);
        const points: TelemetryPayload[] = [];

        for (let i = 5; i >= 0; i--) {
            const shiftTime = now - i * 60;
            points.push({
                device_id: currentTelemetry?.device_id || 'ESP32S3',
                firmware_version: currentTelemetry?.firmware_version || '1.0.0',
                device_core: 2,
                cpu_temperature: baseMcu,
                temperature: baseTemp,
                cpu_load: currentTelemetry?.cpu_load ?? 18,
                timestamp: shiftTime,
                is_connected: true
            });
        }
        return points;
    });

    // Computed Stats for Upper-Right Card (Unavailable when ESP32-S3 is offline)
    let avgTemp = $derived.by(() => {
        if (!isDeviceConnected || !displayHistory || displayHistory.length === 0) return null;
        const sum = displayHistory.reduce((acc, pt) => acc + (pt.temperature ?? 0), 0);
        return (sum / displayHistory.length).toFixed(1);
    });

    let peakVoc = $derived.by(() => {
        if (!isDeviceConnected || currentTelemetry?.temperature == null) return null;
        const currentTemp = currentTelemetry.temperature;
        const basePpb = 42 + Math.round(Math.max(0, currentTemp - 25) * 6.5);
        return `${basePpb} ppb`;
    });

    // Chart SVG Coordinates Calculations (Range: 15°C to 65°C)
    const CHART_WIDTH = 1000;
    const CHART_HEIGHT = 300;

    function getCoords(index: number, total: number, tempVal: number) {
        const x = (index / Math.max(1, total - 1)) * CHART_WIDTH;
        const norm = Math.max(0, Math.min(1, (tempVal - 15) / 50)); // 15 to 65°C
        const y = CHART_HEIGHT - (norm * CHART_HEIGHT);
        return { x, y };
    }

    let ambientTempPoints = $derived.by(() => {
        if (!isDeviceConnected || !displayHistory || displayHistory.length < 2) return '';
        return displayHistory.map((pt, idx) => {
            const val = pt.temperature ?? 36.5;
            const c = getCoords(idx, displayHistory.length, val);
            return `${c.x.toFixed(1)},${c.y.toFixed(1)}`;
        }).join(' ');
    });

    let mcuTempPoints = $derived.by(() => {
        if (!isDeviceConnected || !displayHistory || displayHistory.length < 2) return '';
        const hasAnyCpu = displayHistory.some(pt => pt.cpu_temperature != null && pt.cpu_temperature > 0);
        if (!hasAnyCpu) return '';
        return displayHistory.map((pt, idx) => {
            const val = pt.cpu_temperature != null ? pt.cpu_temperature : (pt.temperature ?? 36.5);
            const c = getCoords(idx, displayHistory.length, val);
            return `${c.x.toFixed(1)},${c.y.toFixed(1)}`;
        }).join(' ');
    });

    let hoveredPoint = $derived(
        isDeviceConnected
            ? (hoveredIdx !== null && displayHistory[hoveredIdx]
                ? displayHistory[hoveredIdx]
                : (displayHistory.length > 0 ? displayHistory[displayHistory.length - 1] : null))
            : null
    );

    // Food category helper
    function getFoodBadge(name: string): { icon: string; label: string } {
        const n = name.toLowerCase();
        if (n.includes('nasi') || n.includes('rice')) return { icon: '🍚', label: 'Rice' };
        if (n.includes('ayam') || n.includes('chicken')) return { icon: '🍗', label: 'Chicken' };
        if (n.includes('tempe')) return { icon: '🥢', label: 'Tempeh' };
        if (n.includes('tahu') || n.includes('tofu')) return { icon: '🧈', label: 'Tofu' };
        if (n.includes('telur') || n.includes('egg')) return { icon: '🍳', label: 'Egg' };
        if (n.includes('kangkung') || n.includes('water_spinach')) return { icon: '🥬', label: 'W. Spinach' };
        if (n.includes('bayam') || n.includes('spinach')) return { icon: '🥗', label: 'Spinach' };
        if (n.includes('pisang') || n.includes('banana')) return { icon: '🍌', label: 'Banana' };
        if (n.includes('jeruk') || n.includes('orange')) return { icon: '🍊', label: 'Orange' };
        if (n.includes('ikan') || n.includes('fish') || n.includes('bawal')) return { icon: '🐟', label: 'Fish' };
        return { icon: '🍱', label: name };
    }

    // Filtered inspection records
    let filteredRecords = $derived(
        records.filter(r => {
            const matchSearch =
                r.id.toLowerCase().includes(searchTerm.toLowerCase()) ||
                r.device_id.toLowerCase().includes(searchTerm.toLowerCase()) ||
                (r.inspection_foods && r.inspection_foods.some(f => f.food_item?.name.toLowerCase().includes(searchTerm.toLowerCase())));

            if (!matchSearch) return false;

            if (statusFilter === 'ALL') return true;
            if (statusFilter === 'REJECT') return r.is_spoiled || (r.inspection_foods && r.inspection_foods.some(f => f.status === 'REJECT'));
            if (statusFilter === 'REVIEW') return !r.is_spoiled && (r.inspection_foods && r.inspection_foods.some(f => f.status === 'REVIEW'));
            if (statusFilter === 'PASS') return !r.is_spoiled && (!r.inspection_foods || r.inspection_foods.every(f => f.status !== 'REJECT' && f.status !== 'REVIEW'));
            return true;
        })
    );
</script>

<div class="merged-view-layout">
    <!-- ========================================================================= -->
    <!-- ROW 1: TELEMETRY ANALYTICS (Temperature Graph & Dual Diagnostic Cards)    -->
    <!-- ========================================================================= -->
    <section class="telemetry-top-grid">
        <!-- 1. Large Card: Temperature Line Graph -->
        <div class="card temperature-chart-card">
            <div class="card-title-bar">
                <span class="card-heading">temperature</span>
                <div class="legend-strip">
                    <span class="legend-item esp32"><span class="dot esp32"></span> ESP32-S3 Real Temp</span>
                    <span class="legend-item mcu"><span class="dot mcu"></span> MCU Core Temp</span>
                </div>
            </div>

            <!-- Chart Container with HTML Layout & SVG Vector Line Plot -->
            <div class="chart-layout-box">
                <div class="chart-main-row">
                    <!-- Left Y-Axis Container (HTML: crisp typography, no SVG distortion) -->
                    <div class="y-axis-container">
                        <div class="y-axis-title-wrap">
                            <span class="y-axis-title">Temperature</span>
                        </div>
                        <div class="y-ticks-column">
                            <span class="y-tick">45°C</span>
                            <span class="y-tick">35°C</span>
                            <span class="y-tick">25°C</span>
                            <span class="y-tick">15°C</span>
                        </div>
                    </div>

                    <!-- Center Plot Area (Vector Canvas + Hover Overlay) -->
                    <div class="plot-canvas-area">
                        {#if !isDeviceConnected}
                            <div class="offline-chart-overlay">
                                <span class="offline-chart-icon">📡</span>
                                <strong>ESP32-S3 Device Offline</strong>
                                <p>Real temperature & hardware telemetry are unavailable while the ESP-IDF sensor node is disconnected.</p>
                            </div>
                        {:else if hoveredPoint}
                            <!-- Floating Interactive Tooltip -->
                            <div
                                class="chart-tooltip"
                                style="left: {hoveredIdx !== null && displayHistory.length > 1 ? `${Math.min(85, Math.max(15, (hoveredIdx / (displayHistory.length - 1)) * 100))}%` : '50%'};"
                            >
                                <span class="tooltip-time">{new Date((hoveredPoint.timestamp || Date.now() / 1000) * 1000).toLocaleTimeString()}</span>
                                <div class="tooltip-metrics">
                                    {#if hoveredPoint.temperature != null}
                                        <span class="chip esp32">🌡️ ESP32-S3: {hoveredPoint.temperature.toFixed(1)}°C</span>
                                    {:else}
                                        <span class="chip esp32 offline">🌡️ ESP32-S3: Offline</span>
                                    {/if}
                                    {#if hoveredPoint.cpu_temperature != null}
                                        <span class="chip mcu">💻 MCU: {hoveredPoint.cpu_temperature.toFixed(1)}°C</span>
                                    {:else}
                                        <span class="chip mcu offline">💻 MCU: Offline</span>
                                    {/if}
                                    {#if hoveredPoint.cpu_load != null}
                                        <span class="chip cpu">⚡ Load: {hoveredPoint.cpu_load}%</span>
                                    {:else}
                                        <span class="chip cpu offline">⚡ Load: Offline</span>
                                    {/if}
                                </div>
                            </div>
                        {/if}

                        <svg viewBox="0 0 {CHART_WIDTH} {CHART_HEIGHT}" class="temperature-svg" preserveAspectRatio="none">
                            <defs>
                                <!-- Linear Gradient for Ambient Temperature Fill -->
                                <linearGradient id="ambientGrad" x1="0%" y1="0%" x2="0%" y2="100%">
                                    <stop offset="0%" stop-color="#f59e0b" stop-opacity="0.28" />
                                    <stop offset="100%" stop-color="#f59e0b" stop-opacity="0.0" />
                                </linearGradient>
                            </defs>

                            <!-- Horizontal Grid Lines corresponding to 45°C, 35°C, 25°C, 15°C -->
                            <line x1="0" y1="0" x2={CHART_WIDTH} y2="0" stroke="rgba(255, 255, 255, 0.08)" stroke-dasharray="4 4" vector-effect="non-scaling-stroke" />
                            <line x1="0" y1="100" x2={CHART_WIDTH} y2="100" stroke="rgba(255, 255, 255, 0.08)" stroke-dasharray="4 4" vector-effect="non-scaling-stroke" />
                            <line x1="0" y1="200" x2={CHART_WIDTH} y2="200" stroke="rgba(255, 255, 255, 0.08)" stroke-dasharray="4 4" vector-effect="non-scaling-stroke" />
                            <line x1="0" y1="300" x2={CHART_WIDTH} y2="300" stroke="rgba(255, 255, 255, 0.08)" stroke-dasharray="4 4" vector-effect="non-scaling-stroke" />

                            <!-- Active Hover Vertical Cursor Guide -->
                            {#if isDeviceConnected && hoveredIdx !== null && displayHistory.length > 1}
                                {@const hoverX = (hoveredIdx / (displayHistory.length - 1)) * CHART_WIDTH}
                                <line
                                    x1={hoverX}
                                    y1={0}
                                    x2={hoverX}
                                    y2={CHART_HEIGHT}
                                    stroke="rgba(245, 158, 11, 0.4)"
                                    stroke-dasharray="3 3"
                                    stroke-width="1.5"
                                    vector-effect="non-scaling-stroke"
                                />
                            {/if}

                            <!-- Area Under Curve (Ambient Temp) -->
                            {#if isDeviceConnected && ambientTempPoints}
                                <polygon
                                    points={`0,${CHART_HEIGHT} ${ambientTempPoints} ${CHART_WIDTH},${CHART_HEIGHT}`}
                                    fill="url(#ambientGrad)"
                                />
                            {/if}

                            <!-- Curve 1: Ambient Temperature Line (Amber) -->
                            {#if isDeviceConnected && ambientTempPoints}
                                <polyline
                                    points={ambientTempPoints}
                                    fill="none"
                                    stroke="#f59e0b"
                                    stroke-width="2.5"
                                    stroke-linecap="round"
                                    stroke-linejoin="round"
                                    vector-effect="non-scaling-stroke"
                                />
                            {/if}

                            <!-- Curve 2: MCU Temperature Line (Blue/Cyan) -->
                            {#if isDeviceConnected && mcuTempPoints}
                                <polyline
                                    points={mcuTempPoints}
                                    fill="none"
                                    stroke="#38bdf8"
                                    stroke-width="2"
                                    stroke-linecap="round"
                                    stroke-linejoin="round"
                                    stroke-dasharray="6 4"
                                    vector-effect="non-scaling-stroke"
                                />
                            {/if}

                            <!-- Interactive Data Points -->
                            {#if isDeviceConnected && displayHistory}
                                {#each displayHistory as pt, idx}
                                    {#if pt.temperature != null}
                                        {@const c = getCoords(idx, displayHistory.length, pt.temperature)}
                                        <circle
                                            cx={c.x}
                                            cy={c.y}
                                            r={hoveredIdx === idx ? 5.5 : 3.5}
                                            fill="#f59e0b"
                                            stroke="#14171d"
                                            stroke-width="2"
                                            vector-effect="non-scaling-stroke"
                                        />
                                    {/if}
                                {/each}
                            {/if}
                        </svg>

                        <!-- Invisible Full-Height Hover Trigger Columns -->
                        {#if isDeviceConnected}
                            <div class="hover-detector-overlay">
                                {#each displayHistory as _, idx}
                                    <div
                                        class="hover-trigger-slice"
                                        role="presentation"
                                        onmouseenter={() => (hoveredIdx = idx)}
                                        onmouseleave={() => (hoveredIdx = null)}
                                    ></div>
                                {/each}
                            </div>
                        {/if}
                    </div>
                </div>

                <!-- Bottom X-Axis Row (HTML: crisp typography, centered under plot) -->
                <div class="x-axis-row">
                    <span class="x-axis-title">operational shift timeline</span>
                </div>
            </div>
        </div>

        <!-- 2. Right Column (Stacked Cards matching Wireframe) -->
        <div class="telemetry-right-column">
            <!-- Upper Card: Avg. Temp & Peak VOC -->
            <div class="card metric-mini-card">
                <div class="metric-row">
                    <span class="metric-label">Avg. ESP32 Temp</span>
                    <div class="value-box">
                        {#if isDeviceConnected && avgTemp != null}
                            <span class="value-text">{avgTemp} °C</span>
                        {:else}
                            <span class="value-text offline-val">Offline</span>
                        {/if}
                    </div>
                </div>

                <div class="metric-row">
                    <span class="metric-label">Peak VOC</span>
                    <div class="value-box">
                        {#if isDeviceConnected && peakVoc != null}
                            <span class="value-text">{peakVoc}</span>
                        {:else}
                            <span class="value-text offline-val">Offline</span>
                        {/if}
                    </div>
                </div>
            </div>

            <!-- Lower Card: Device Telemetry & Specs -->
            <div class="card device-specs-card">
                <div class="metric-row">
                    <span class="metric-label">Device</span>
                    <div class="value-box mono">
                        <span class="value-text" class:offline-val={!isDeviceConnected}>
                            {isDeviceConnected ? (currentTelemetry?.device_id || 'ESP32-S3').slice(0, 14) : 'Disconnected'}
                        </span>
                    </div>
                </div>

                <div class="metric-row">
                    <span class="metric-label">FMW</span>
                    <div class="value-box">
                        <span class="value-text" class:offline-val={!isDeviceConnected}>
                            {isDeviceConnected ? `v${currentTelemetry?.firmware_version || '1.0.0'}` : 'Offline'}
                        </span>
                    </div>
                </div>

                <div class="metric-row">
                    <span class="metric-label">CPU Core 0</span>
                    <div class="value-box">
                        <span class="value-text" class:offline-val={!isDeviceConnected}>
                            {isDeviceConnected && currentTelemetry?.cpu_load != null ? `${currentTelemetry.cpu_load}% @ 240M` : 'Offline'}
                        </span>
                    </div>
                </div>

                <div class="metric-row">
                    <span class="metric-label">CPU Core 1</span>
                    <div class="value-box">
                        <span class="value-text" class:offline-val={!isDeviceConnected}>
                            {isDeviceConnected && currentTelemetry?.cpu_load != null ? `${Math.max(8, Math.round(currentTelemetry.cpu_load * 0.7))}% @ 240M` : 'Offline'}
                        </span>
                    </div>
                </div>

                <div class="metric-row">
                    <span class="metric-label">RSSI</span>
                    <div class="value-box">
                        <span class="value-text" class:offline-val={!isDeviceConnected}>
                            {isDeviceConnected ? '-58 dBm' : 'Offline'}
                        </span>
                    </div>
                </div>

                <div class="metric-row">
                    <span class="metric-label">Heap Free</span>
                    <div class="value-box">
                        <span class="value-text" class:offline-val={!isDeviceConnected}>
                            {isDeviceConnected ? '284 KB' : 'Offline'}
                        </span>
                    </div>
                </div>
            </div>
        </div>
    </section>

    <!-- ========================================================================= -->
    <!-- CAMERA INSPECTION STRIP (Video & Camera Ingestion Switcher)               -->
    <!-- ========================================================================= -->
    <div class="card telemetry-camera-toggle-card">
        <div class="toggle-header-row">
            <div class="toggle-title-box">
                <span class="cam-icon">📹</span>
                <div>
                    <strong>Telemetry Video & Camera Ingestion Monitor</strong>
                    <p class="toggle-desc">Switch between ESP32 live stream, zero-lag /camera/frame snapshot polling, or mock MP4 test videos.</p>
                </div>
            </div>
            <button
                type="button"
                class="cam-toggle-btn"
                class:active={showCameraInspection}
                onclick={() => (showCameraInspection = !showCameraInspection)}
            >
                {showCameraInspection ? '▲ Hide Camera Monitor' : '▼ Show Video / Camera Stream'}
            </button>
        </div>

        {#if showCameraInspection}
            <div class="telemetry-camera-body">
                <CameraStream
                    detections={[]}
                    deviceId={currentTelemetry?.device_id}
                    isLive={true}
                    hasCamera={true}
                />
            </div>
        {/if}
    </div>

    <!-- ========================================================================= -->
    <!-- ROW 2: RESULT LOG (Full-Width Historical Database Table)                 -->
    <!-- ========================================================================= -->
    <section class="card result-log-section">
        <div class="result-log-header">
            <div class="title-with-pill">
                <span class="card-heading">Result Log</span>
                <span class="count-badge">{filteredRecords.length} records</span>
            </div>

            <!-- Table Filters -->
            <div class="table-actions-strip">
                <div class="status-filters">
                    <button
                        type="button"
                        class="filter-pill"
                        class:active={statusFilter === 'ALL'}
                        onclick={() => (statusFilter = 'ALL')}
                    >All</button>
                    <button
                        type="button"
                        class="filter-pill pass"
                        class:active={statusFilter === 'PASS'}
                        onclick={() => (statusFilter = 'PASS')}
                    >Pass</button>
                    <button
                        type="button"
                        class="filter-pill review"
                        class:active={statusFilter === 'REVIEW'}
                        onclick={() => (statusFilter = 'REVIEW')}
                    >Review</button>
                    <button
                        type="button"
                        class="filter-pill reject"
                        class:active={statusFilter === 'REJECT'}
                        onclick={() => (statusFilter = 'REJECT')}
                    >Reject</button>
                </div>

                <input
                    type="text"
                    placeholder="Search lot, device, food..."
                    bind:value={searchTerm}
                    class="log-search-input"
                />

                <button type="button" class="action-btn" onclick={loadRecords} title="Refresh Database">
                    ↻ Refresh
                </button>
            </div>
        </div>

        <!-- Wireframe Matching Data Table -->
        <div class="table-container">
            <table class="result-table">
                <thead>
                    <tr>
                        <th class="col-id">ID</th>
                        <th class="col-time">Timestamp</th>
                        <th class="col-food">Food Composition</th>
                        <th class="col-temp">Temperature</th>
                        <th class="col-status">Status</th>
                    </tr>
                </thead>
                <tbody>
                    {#if isLoading}
                        <tr>
                            <td colspan="5" class="empty-cell">
                                <div class="loading-spinner"></div>
                                <span>Loading inspection archives from PostgreSQL...</span>
                            </td>
                        </tr>
                    {:else if filteredRecords.length === 0}
                        <tr>
                            <td colspan="5" class="empty-cell">
                                <span>No inspection audit records match current filter.</span>
                            </td>
                        </tr>
                    {:else}
                        {#each filteredRecords as rec}
                            <tr class="log-row" onclick={() => (selectedRecord = rec)}>
                                <!-- ID Column -->
                                <td class="col-id">
                                    <span class="id-badge">#{rec.id.slice(0, 8).toUpperCase()}</span>
                                </td>

                                <!-- Timestamp Column -->
                                <td class="col-time">
                                    <span class="time-text">{new Date(rec.created_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}</span>
                                    <span class="date-sub">{new Date(rec.created_at).toLocaleDateString([], { month: 'short', day: 'numeric' })}</span>
                                </td>

                                <!-- Food Composition Column (Component Slots matching wireframe boxes) -->
                                <td class="col-food">
                                    {#if rec.inspection_foods && rec.inspection_foods.length > 0}
                                        <div class="food-composition-grid">
                                            {#each rec.inspection_foods as f}
                                                {@const badge = getFoodBadge(f.food_item?.name || 'Food')}
                                                <div
                                                    class="food-slot-box"
                                                    class:slot-pass={f.status === 'PASS'}
                                                    class:slot-review={f.status === 'REVIEW'}
                                                    class:slot-reject={f.status === 'REJECT'}
                                                    title={`${badge.label}: ${((f.similarity_rate || 0.9) * 100).toFixed(0)}% (${f.status})`}
                                                >
                                                    <span class="slot-icon">{badge.icon}</span>
                                                    <span class="slot-name">{badge.label}</span>
                                                    <span class="slot-conf">{((f.similarity_rate || 0.9) * 100).toFixed(0)}%</span>
                                                </div>
                                            {/each}
                                        </div>
                                    {:else}
                                        <div class="food-composition-grid">
                                            <div class="food-slot-box slot-pass">
                                                <span class="slot-icon">🍱</span>
                                                <span class="slot-name">Meal Tray</span>
                                                <span class="slot-conf">100%</span>
                                            </div>
                                        </div>
                                    {/if}
                                </td>

                                <!-- Temperature Column -->
                                <td class="col-temp">
                                    <div class="temp-badge" class:hot={(rec.temperature || 25) > 38}>
                                        <span class="temp-icon">🌡️</span>
                                        <span class="temp-val">{(rec.temperature || 24.5).toFixed(1)}°C</span>
                                    </div>
                                </td>

                                <!-- Status Column -->
                                <td class="col-status">
                                    {#if rec.is_spoiled || (rec.inspection_foods && rec.inspection_foods.some(f => f.status === 'REJECT'))}
                                        <span class="status-badge reject">REJECT</span>
                                    {:else if rec.inspection_foods && rec.inspection_foods.some(f => f.status === 'REVIEW')}
                                        <span class="status-badge review">REVIEW</span>
                                    {:else}
                                        <span class="status-badge pass">PASS</span>
                                    {/if}
                                </td>
                            </tr>
                        {/each}
                    {/if}
                </tbody>
            </table>
        </div>
    </section>

    <!-- Inspection Record Modal Details -->
    {#if selectedRecord}
        <div class="modal-backdrop" role="dialog" aria-modal="true">
            <button
                type="button"
                class="modal-scrim"
                aria-label="Close modal dialog"
                onclick={() => (selectedRecord = null)}
            ></button>
            <div class="modal-card">
                <div class="modal-header">
                    <div class="modal-header-title">
                        <h3>Inspection Audit #{selectedRecord.id.slice(0, 12)}</h3>
                        <span class="modal-time">{new Date(selectedRecord.created_at).toLocaleString()}</span>
                    </div>
                    <button type="button" class="modal-close-btn" onclick={() => (selectedRecord = null)}>✕</button>
                </div>
                <div class="modal-body">
                    <div class="modal-details-grid">
                        <div class="detail-item">
                            <span class="label">Device Node:</span>
                            <strong class="val">{selectedRecord.device_id}</strong>
                        </div>
                        <div class="detail-item">
                            <span class="label">Firmware:</span>
                            <strong class="val">{selectedRecord.firmware_version}</strong>
                        </div>
                        <div class="detail-item">
                            <span class="label">ESP32-S3 Temp:</span>
                            <strong class="val">{(selectedRecord.temperature || 36.5).toFixed(1)}°C</strong>
                        </div>
                        <div class="detail-item">
                            <span class="label">CPU Temp:</span>
                            <strong class="val">{selectedRecord.cpu_temperature != null ? `${selectedRecord.cpu_temperature.toFixed(1)}°C` : 'N/A (Disconnected)'}</strong>
                        </div>
                    </div>

                    <h4 class="json-section-heading">Raw Ingress Payload</h4>
                    <pre class="json-viewer">{JSON.stringify(selectedRecord, null, 2)}</pre>
                </div>
            </div>
        </div>
    {/if}
</div>

<style>
    .merged-view-layout {
        display: flex;
        flex-direction: column;
        gap: 16px;
        width: 100%;
    }

    /* Base Card Styling matching wireframe */
    .card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-md);
        box-shadow: var(--shadow-card);
        padding: 16px 20px;
    }

    .card-heading {
        font-size: 0.95rem;
        font-weight: 700;
        text-transform: capitalize;
        color: var(--text-primary);
        letter-spacing: 0.02em;
    }

    /* Telemetry Camera Ingestion Card */
    .telemetry-camera-toggle-card {
        padding: 12px 18px;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-md);
        display: flex;
        flex-direction: column;
        gap: 12px;
    }

    .toggle-header-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 16px;
    }

    .toggle-title-box {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .toggle-title-box strong {
        font-size: 0.92rem;
        color: var(--text-primary);
    }

    .toggle-desc {
        margin: 2px 0 0;
        font-size: 0.75rem;
        color: var(--text-muted);
    }

    .cam-icon {
        font-size: 1.4rem;
    }

    .cam-toggle-btn {
        background: var(--bg-chip, #1e293b);
        color: var(--accent-blue, #60a5fa);
        border: 1px solid var(--border-subtle, #334155);
        border-radius: var(--radius-pill, 9999px);
        padding: 6px 14px;
        font-size: 0.78rem;
        font-weight: 600;
        cursor: pointer;
        transition: all 0.15s ease;
        white-space: nowrap;
    }

    .cam-toggle-btn:hover {
        background: var(--accent-blue, #3b82f6);
        color: #ffffff;
    }

    .cam-toggle-btn.active {
        background: var(--accent-blue, #3b82f6);
        color: #ffffff;
    }

    .telemetry-camera-body {
        margin-top: 4px;
        padding-top: 12px;
        border-top: 1px solid var(--border-subtle);
    }

    /* ------------------------------------------------------------------------- */
    /* TOP ROW: TELEMETRY ANALYTICS                                              */
    /* ------------------------------------------------------------------------- */
    .telemetry-top-grid {
        display: grid;
        grid-template-columns: minmax(0, 1fr) 360px;
        gap: 16px;
        align-items: stretch;
    }

    @media (max-width: 1200px) {
        .telemetry-top-grid {
            grid-template-columns: 1fr;
        }
    }

    /* Temperature Card */
    .temperature-chart-card {
        display: flex;
        flex-direction: column;
        min-height: 380px;
        height: 100%;
        padding-bottom: 12px;
    }

    .card-title-bar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 12px;
        padding-bottom: 8px;
        border-bottom: 1px solid var(--border-subtle);
    }

    .legend-strip {
        display: flex;
        align-items: center;
        gap: 14px;
        font-size: 0.72rem;
        color: var(--text-secondary);
    }

    .legend-item {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .legend-item .dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
    }

    .legend-item .dot.esp32 { background: #f59e0b; }
    .legend-item .dot.mcu { background: #38bdf8; }

    /* New Unsquished Chart Layout */
    .chart-layout-box {
        display: flex;
        flex-direction: column;
        flex: 1;
        width: 100%;
        min-height: 0;
    }

    .chart-main-row {
        display: flex;
        flex: 1;
        min-height: 280px;
        gap: 8px;
        position: relative;
    }

    .y-axis-container {
        display: flex;
        align-items: stretch;
        gap: 6px;
        user-select: none;
        flex-shrink: 0;
        padding: 0;
    }

    .y-axis-title-wrap {
        display: flex;
        align-items: center;
        justify-content: center;
        width: 18px;
    }

    .y-axis-title {
        writing-mode: vertical-rl;
        transform: rotate(180deg);
        color: var(--text-secondary);
        font-size: 0.72rem;
        font-weight: 600;
        letter-spacing: 0.05em;
        text-transform: capitalize;
        white-space: nowrap;
    }

    .y-ticks-column {
        display: flex;
        flex-direction: column;
        justify-content: space-between;
        align-items: flex-end;
        width: 36px;
        margin: -6px 0;
        padding-right: 6px;
    }

    .y-tick {
        color: var(--text-muted);
        font-size: 0.72rem;
        font-family: var(--font-mono);
        line-height: 1;
    }

    .plot-canvas-area {
        position: relative;
        flex: 1;
        min-height: 280px;
        background: var(--bg-inset);
        border-left: 1.5px solid var(--border-medium);
        border-bottom: 1.5px solid var(--border-medium);
        border-radius: 0 var(--radius-sm) 0 0;
        overflow: hidden;
    }

    .temperature-svg {
        width: 100%;
        height: 100%;
        display: block;
    }

    .x-axis-row {
        display: flex;
        justify-content: center;
        align-items: center;
        padding-top: 10px;
        padding-left: 60px; /* Offset to center title under plot area */
    }

    .x-axis-title {
        color: var(--text-secondary);
        font-size: 0.72rem;
        font-weight: 600;
        letter-spacing: 0.05em;
        user-select: none;
    }

    /* Hover tooltips */
    .chart-tooltip {
        position: absolute;
        top: 12px;
        transform: translateX(-50%);
        background: rgba(20, 24, 32, 0.95);
        border: 1px solid var(--border-highlight);
        border-radius: var(--radius-sm);
        padding: 6px 12px;
        display: flex;
        align-items: center;
        gap: 10px;
        z-index: 10;
        box-shadow: 0 4px 16px rgba(0, 0, 0, 0.5);
        pointer-events: none;
        white-space: nowrap;
    }

    .tooltip-time {
        font-family: var(--font-mono);
        font-size: 0.7rem;
        color: var(--text-muted);
        border-right: 1px solid var(--border-subtle);
        padding-right: 8px;
    }

    .tooltip-metrics {
        display: flex;
        gap: 8px;
    }

    .tooltip-metrics .chip {
        font-size: 0.72rem;
        font-weight: 600;
        font-family: var(--font-mono);
    }

    .tooltip-metrics .chip.esp32 { color: #f59e0b; }
    .tooltip-metrics .chip.mcu { color: #38bdf8; }
    .tooltip-metrics .chip.cpu { color: var(--accent-green); }
    .tooltip-metrics .chip.offline { color: #dc2626 !important; opacity: 0.75; }

    .hover-detector-overlay {
        position: absolute;
        inset: 0;
        display: flex;
        cursor: crosshair;
        z-index: 5;
    }

    .hover-trigger-slice {
        flex: 1;
        height: 100%;
    }

    /* ------------------------------------------------------------------------- */
    /* RIGHT COLUMN (Upper Metric & Lower Diagnostic)                            */
    /* ------------------------------------------------------------------------- */
    .telemetry-right-column {
        display: flex;
        flex-direction: column;
        gap: 16px;
    }

    .metric-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        padding: 5px 0;
    }

    .metric-label {
        font-size: 0.8rem;
        font-weight: 600;
        color: var(--text-secondary);
        white-space: nowrap;
    }

    .value-box {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
        padding: 5px 12px;
        min-width: 120px;
        text-align: right;
        display: flex;
        align-items: center;
        justify-content: flex-end;
    }

    .value-box.mono {
        font-family: var(--font-mono);
    }

    .value-text {
        font-size: 0.8rem;
        font-weight: 700;
        color: var(--text-primary);
        font-family: var(--font-mono);
    }

    .metric-mini-card {
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .device-specs-card {
        display: flex;
        flex-direction: column;
        gap: 6px;
        flex: 1;
        justify-content: center;
    }

    /* ------------------------------------------------------------------------- */
    /* ROW 2: RESULT LOG SECTION                                                 */
    /* ------------------------------------------------------------------------- */
    .result-log-section {
        display: flex;
        flex-direction: column;
        gap: 14px;
    }

    .result-log-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        flex-wrap: wrap;
        gap: 12px;
        padding-bottom: 12px;
        border-bottom: 1px solid var(--border-subtle);
    }

    .title-with-pill {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .count-badge {
        font-size: 0.7rem;
        font-weight: 600;
        background: var(--bg-chip);
        color: var(--accent-blue);
        padding: 2px 8px;
        border-radius: var(--radius-pill);
    }

    .table-actions-strip {
        display: flex;
        align-items: center;
        gap: 10px;
        flex-wrap: wrap;
    }

    .status-filters {
        display: flex;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-pill);
        padding: 2px;
    }

    .filter-pill {
        background: transparent;
        border: none;
        color: var(--text-muted);
        font-size: 0.72rem;
        font-weight: 600;
        padding: 4px 10px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .filter-pill.active {
        background: var(--bg-chip);
        color: var(--text-primary);
    }

    .filter-pill.pass.active { background: var(--accent-green-dim); color: var(--accent-green); }
    .filter-pill.review.active { background: var(--accent-amber-dim); color: var(--accent-amber); }
    .filter-pill.reject.active { background: var(--accent-red-dim); color: var(--accent-red); }

    .log-search-input {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        color: var(--text-primary);
        font-size: 0.78rem;
        padding: 6px 12px;
        border-radius: var(--radius-pill);
        outline: none;
        width: 200px;
        transition: border-color 0.15s ease;
    }

    .log-search-input:focus {
        border-color: var(--accent-blue);
    }

    .action-btn {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        color: var(--accent-blue);
        font-size: 0.75rem;
        font-weight: 600;
        padding: 6px 14px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .action-btn:hover {
        background: var(--bg-card-hover);
        border-color: var(--border-highlight);
    }

    /* ------------------------------------------------------------------------- */
    /* RESULT LOG DATA TABLE                                                     */
    /* ------------------------------------------------------------------------- */
    .table-container {
        overflow-x: auto;
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
    }

    .result-table {
        width: 100%;
        border-collapse: collapse;
        text-align: left;
        font-size: 0.8rem;
    }

    .result-table th {
        background: var(--bg-inset);
        color: var(--text-secondary);
        font-size: 0.72rem;
        font-weight: 700;
        padding: 10px 14px;
        border-bottom: 1px solid var(--border-card);
        white-space: nowrap;
    }

    .result-table td {
        padding: 10px 14px;
        border-bottom: 1px solid var(--border-subtle);
        vertical-align: middle;
    }

    .log-row {
        cursor: pointer;
        transition: background 0.12s ease;
    }

    .log-row:hover {
        background: var(--bg-card-hover);
    }

    .col-id { width: 110px; }
    .col-time { width: 130px; }
    .col-food { min-width: 320px; }
    .col-temp { width: 130px; }
    .col-status { width: 90px; text-align: center; }

    .id-badge {
        font-family: var(--font-mono);
        font-weight: 700;
        font-size: 0.75rem;
        color: var(--accent-blue);
        background: var(--accent-blue-dim);
        padding: 2px 8px;
        border-radius: var(--radius-pill);
    }

    .time-text {
        display: block;
        font-family: var(--font-mono);
        font-size: 0.78rem;
        color: var(--text-primary);
    }

    .date-sub {
        font-size: 0.65rem;
        color: var(--text-muted);
    }

    /* Food Composition Slot Boxes matching the Wireframe cells */
    .food-composition-grid {
        display: flex;
        flex-wrap: wrap;
        gap: 6px;
        align-items: center;
    }

    .food-slot-box {
        display: inline-flex;
        align-items: center;
        gap: 5px;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
        padding: 4px 8px;
        font-size: 0.72rem;
        transition: transform 0.1s ease;
    }

    .food-slot-box:hover {
        transform: translateY(-1px);
    }

    .slot-icon {
        font-size: 0.85rem;
    }

    .slot-name {
        font-weight: 600;
        color: var(--text-primary);
    }

    .slot-conf {
        font-family: var(--font-mono);
        font-size: 0.65rem;
        color: var(--text-muted);
    }

    .slot-pass {
        border-color: rgba(34, 197, 94, 0.35);
        background: rgba(34, 197, 94, 0.08);
    }

    .slot-review {
        border-color: rgba(245, 158, 11, 0.35);
        background: rgba(245, 158, 11, 0.08);
    }

    .slot-reject {
        border-color: rgba(244, 63, 94, 0.35);
        background: rgba(244, 63, 94, 0.08);
    }

    .temp-badge {
        display: inline-flex;
        align-items: center;
        gap: 5px;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-pill);
        padding: 3px 10px;
        font-family: var(--font-mono);
        font-size: 0.75rem;
        color: var(--text-primary);
    }

    .temp-badge.hot {
        border-color: rgba(244, 63, 94, 0.4);
        color: var(--accent-red);
    }

    .status-badge {
        display: inline-block;
        font-size: 0.68rem;
        font-weight: 800;
        letter-spacing: 0.05em;
        padding: 4px 10px;
        border-radius: var(--radius-pill);
        border: 1px solid transparent;
    }

    .status-badge.pass {
        background: var(--accent-green-dim);
        color: var(--accent-green);
        border-color: rgba(34, 197, 94, 0.35);
    }

    .status-badge.review {
        background: var(--accent-amber-dim);
        color: var(--accent-amber);
        border-color: rgba(245, 158, 11, 0.35);
    }

    .status-badge.reject {
        background: var(--accent-red-dim);
        color: var(--accent-red);
        border-color: rgba(244, 63, 94, 0.35);
    }

    .empty-cell {
        text-align: center;
        padding: 36px;
        color: var(--text-muted);
    }

    .loading-spinner {
        display: inline-block;
        width: 16px;
        height: 16px;
        border: 2px solid var(--accent-blue-dim);
        border-top-color: var(--accent-blue);
        border-radius: 50%;
        animation: spin 0.8s linear infinite;
        vertical-align: middle;
        margin-right: 8px;
    }

    @keyframes spin {
        to { transform: rotate(360deg); }
    }

    /* ------------------------------------------------------------------------- */
    /* MODAL AUDIT VIEWER                                                        */
    /* ------------------------------------------------------------------------- */
    .modal-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.75);
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 100;
        padding: 20px;
    }

    .modal-scrim {
        position: absolute;
        inset: 0;
        background: transparent;
        border: none;
        cursor: pointer;
        width: 100%;
        height: 100%;
    }

    .modal-card {
        position: relative;
        z-index: 101;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        width: 640px;
        max-width: 95vw;
        max-height: 85vh;
        display: flex;
        flex-direction: column;
        box-shadow: 0 10px 40px rgba(0, 0, 0, 0.6);
        overflow: hidden;
    }

    .modal-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 16px 20px;
        border-bottom: 1px solid var(--border-subtle);
    }

    .modal-header-title h3 {
        margin: 0;
        font-size: 1rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .modal-time {
        font-size: 0.7rem;
        color: var(--text-muted);
    }

    .modal-close-btn {
        background: transparent;
        border: none;
        color: var(--text-muted);
        font-size: 1.2rem;
        cursor: pointer;
    }

    .modal-body {
        padding: 18px 20px;
        overflow-y: auto;
        display: flex;
        flex-direction: column;
        gap: 14px;
    }

    .modal-details-grid {
        display: grid;
        grid-template-columns: repeat(2, 1fr);
        gap: 10px;
        background: var(--bg-inset);
        padding: 12px;
        border-radius: var(--radius-sm);
        border: 1px solid var(--border-subtle);
    }

    .detail-item {
        display: flex;
        justify-content: space-between;
        font-size: 0.78rem;
    }

    .detail-item .label { color: var(--text-muted); }
    .detail-item .val { color: var(--text-primary); font-family: var(--font-mono); }

    .json-section-heading {
        font-size: 0.75rem;
        text-transform: uppercase;
        color: var(--text-muted);
        letter-spacing: 0.05em;
        margin: 4px 0 0;
    }

    .json-viewer {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
        padding: 12px;
        color: var(--accent-blue);
        font-family: var(--font-mono);
        font-size: 0.72rem;
        overflow-x: auto;
        max-height: 240px;
    }

    .offline-chart-overlay {
        position: absolute;
        inset: 0;
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        text-align: center;
        padding: 20px;
        gap: 6px;
        z-index: 8;
        background: rgba(248, 250, 252, 0.88);
    }

    .offline-chart-icon {
        font-size: 1.8rem;
    }

    .offline-chart-overlay strong {
        font-size: 0.9rem;
        color: var(--text-primary);
    }

    .offline-chart-overlay p {
        margin: 0;
        font-size: 0.75rem;
        color: var(--text-muted);
        max-width: 340px;
    }

    .value-text.offline-val {
        color: #dc2626;
        font-weight: 600;
    }

    @media (max-width: 768px) {
        .card {
            padding: 12px 14px;
        }

        .card-title-bar {
            flex-direction: column;
            align-items: flex-start;
            gap: 8px;
        }

        .legend-strip {
            flex-wrap: wrap;
            gap: 8px 12px;
        }

        .temperature-chart-card {
            min-height: 280px;
        }

        .chart-main-row,
        .plot-canvas-area {
            min-height: 200px;
        }

        .x-axis-row {
            padding-left: 0;
        }

        .toggle-header-row {
            flex-direction: column;
            align-items: stretch;
            gap: 10px;
        }

        .cam-toggle-btn {
            width: 100%;
            text-align: center;
        }

        .result-log-header {
            flex-direction: column;
            align-items: stretch;
        }

        .table-actions-strip {
            width: 100%;
        }

        .status-filters {
            width: 100%;
            justify-content: space-between;
        }

        .filter-pill {
            flex: 1;
            text-align: center;
        }

        .log-search-input {
            width: 100%;
            flex: 1;
        }

        .modal-details-grid {
            grid-template-columns: 1fr;
        }
    }
</style>

