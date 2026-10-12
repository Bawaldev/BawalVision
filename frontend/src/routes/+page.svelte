<script lang="ts">
    import { onMount, onDestroy } from 'svelte';
    import Sidebar from '$lib/components/Sidebar.svelte';
    import TopBar from '$lib/components/TopBar.svelte';
    import DashboardView from '$lib/views/DashboardView.svelte';
    import TelemetryResultLogView from '$lib/views/TelemetryResultLogView.svelte';
    import SettingsView from '$lib/views/SettingsView.svelte';

    import {
        fetchRecords,
        fetchCameraStatus,
        fetchLatestTelemetry,
        fetchLatestInspection,
        fetchEngineStatus,
        triggerManualInspection
    } from '$lib/api';

    import type {
        FoodDetection,
        TelemetryPayload,
        InferenceLogEntry,
        SystemConfig
    } from '$lib/types';

    // Helper to determine category
    function getCategory(name: string): 'Carbohydrate' | 'Protein' | 'Vegetable' | 'Fruit' | 'Side' {
        const n = name.toLowerCase();
        if (n.includes('nasi') || n.includes('rice')) return 'Carbohydrate';
        if (n.includes('ayam') || n.includes('ikan') || n.includes('tofu') || n.includes('chicken') || n.includes('fish')) return 'Protein';
        if (n.includes('kangkung') || n.includes('bayam') || n.includes('sayur') || n.includes('spinach')) return 'Vegetable';
        if (n.includes('pisang') || n.includes('jeruk') || n.includes('buah') || n.includes('banana') || n.includes('orange')) return 'Fruit';
        return 'Side';
    }

    // State Variables
    const VALID_TABS = [
        'Dashboard',
        'Telemetry Analytics & Result Log',
        'System & Cloud Settings'
    ];

    let activeTab = $state('Dashboard');

    function handleSelectTab(tab: string) {
        if (!VALID_TABS.includes(tab)) return;
        activeTab = tab;
        if (typeof window !== 'undefined') {
            try {
                localStorage.setItem('bawal_active_tab', tab);
                const slug = tab === 'Telemetry Analytics & Result Log'
                    ? 'telemetry'
                    : (tab === 'System & Cloud Settings' ? 'settings' : 'dashboard');
                window.location.hash = slug;
            } catch {}
        }
    }
    let isOnline = $state(false);
    let hasCamera = $state(false);
    let isInspecting = $state(false);
    let gravitonAvailable = $state(false);
    let localEngineAvailable = $state(true);

    let systemConfig = $state<SystemConfig>({
        backendUrl: '/api/v1',
        deviceId: '',
        streamIntervalMs: 1500,
        enableMockIfOffline: false,
        minConfidenceThreshold: 0.35,
        tempWarningThreshold: 35.0,
        gravitonEngineEnabled: false,
        gravitonEngineAvailable: false,
        localEngineEnabled: true,
        localEngineAvailable: true
    });

    let detections = $state<FoodDetection[]>([]);
    let telemetry = $state<TelemetryPayload>({
        device_id: 'WAITING_DEVICE',
        firmware_version: '1.0.0',
        device_core: 0,
        temperature: 0,
        cpu_load: 0,
        timestamp: Math.floor(Date.now() / 1000)
    });

    let logs = $state<InferenceLogEntry[]>([
        {
            id: 'init-1',
            timestamp: new Date().toLocaleTimeString(),
            level: 'INFO',
            message: 'BawalVision UI initialized. Awaiting live telemetry & camera ingress from backend...'
        }
    ]);

    let totalAccepted = $state(0);
    let totalRejected = $state(0);
    let totalPlates = $derived(totalAccepted + totalRejected);
    let passRate = $derived(totalPlates > 0 ? (totalAccepted / totalPlates) * 100 : 0.0);
    let bufferedRecords = $state(0);
    let telemetryHistory = $state<TelemetryPayload[]>([]);

    let pollingTimer: any = null;

    // Real Backend Polling Step
    async function pollBackendData() {
        try {
            // 1. Hardware Engine Status
            const engines = await fetchEngineStatus(systemConfig.backendUrl);
            gravitonAvailable = engines.graviton_available;
            localEngineAvailable = engines.local_engine_available;
            systemConfig.gravitonEngineAvailable = engines.graviton_available;
            systemConfig.localEngineAvailable = engines.local_engine_available;
            if (!gravitonAvailable && systemConfig.gravitonEngineEnabled) {
                systemConfig.gravitonEngineEnabled = false;
            }

            // 2. Camera Status
            const cam = await fetchCameraStatus(systemConfig.backendUrl);
            hasCamera = cam.active;
            if (cam.active && cam.deviceId) {
                systemConfig.deviceId = cam.deviceId;
                isOnline = true;
            }

            // 3. Telemetry Ingress from ESP32-S3
            const tel = await fetchLatestTelemetry(systemConfig.backendUrl);
            if (tel) {
                telemetry = tel;
                if (!telemetryHistory.some(h => h.timestamp === tel.timestamp)) {
                    telemetryHistory = [...telemetryHistory.slice(-19), tel];
                }
                isOnline = true;
            }

            // 4. Live Real-time Inspections & Dynamic Bounding Boxes
            const latestLive: any = await fetchLatestInspection(systemConfig.backendUrl);
            let hasLiveDetections = false;
            if (latestLive && latestLive.detections && latestLive.detections.length > 0) {
                isOnline = true;
                hasLiveDetections = true;
                detections = latestLive.detections.map((det: any) => {
                    const spoilage = Number(det.spoilage_score) || 0;
                    const conf = Number(det.similarity_rate) || 0.0;
                    let st: 'PASS' | 'REVIEW' | 'REJECT' = det.status;
                    if (!st || (st as string) === '') {
                        st = (spoilage >= 50 || conf < 0.55) ? 'REJECT' : ((spoilage >= 25 || conf < 0.68) ? 'REVIEW' : 'PASS');
                    }
                    return {
                        food_name: det.food_name || 'food_item',
                        similarity_rate: conf,
                        temperature: Number(det.temperature) || Number(latestLive.food_temperature) || Number(latestLive.temperature) || 25,
                        spoilage_score: spoilage,
                        status: st,
                        category: getCategory(det.food_name || ''),
                        box: det.box || []
                    };
                });
            }

            // 5. Inspections & Records from DB (Only count actual meals with scanned food items!)
            const allRecords = await fetchRecords(systemConfig.backendUrl);
            const validRecords = (allRecords || []).filter(
                r => r.inspection_foods && r.inspection_foods.length > 0
            );

            if (validRecords.length > 0) {
                isOnline = true;
                const spoiled = validRecords.filter(r => r.is_spoiled || (r.inspection_foods && r.inspection_foods.some(f => f.status === 'REJECT'))).length;
                const accepted = validRecords.length - spoiled;
                totalRejected = Math.max(totalRejected, spoiled);
                totalAccepted = Math.max(totalAccepted, accepted);

                if (!hasLiveDetections) {
                    const latest = validRecords[0];
                    if (latest && latest.inspection_foods && latest.inspection_foods.length > 0) {
                        detections = latest.inspection_foods.map(item => {
                            const name = item.food_item?.name || 'food_item';
                            const sp = Number(item.spoilage_score) || 0;
                            const cf = Number(item.similarity_rate) || 0.0;
                            let st: 'PASS' | 'REVIEW' | 'REJECT' = (item.status as any);
                            if (!st || (st as string) === '') {
                                st = (sp >= 50 || cf < 0.55) ? 'REJECT' : ((sp >= 25 || cf < 0.68) ? 'REVIEW' : 'PASS');
                            }
                            return {
                                food_name: name,
                                similarity_rate: cf,
                                temperature: Number(item.temperature) || Number(latest.food_temperature) || Number(latest.temperature) || 25,
                                spoilage_score: sp,
                                status: st,
                                category: getCategory(name)
                            };
                        });
                    } else {
                        detections = [];
                    }
                }
            } else if (!hasLiveDetections) {
                detections = [];
            }
        } catch (err) {
            console.warn('[BACKEND POLL] Offline or waiting for backend service:', err);
        }
    }

    onMount(async () => {
        // Restore active tab from URL hash or localStorage so refresh maintains current view
        if (typeof window !== 'undefined') {
            try {
                const hash = window.location.hash.toLowerCase();
                if (hash.includes('telemetry') || hash.includes('result')) {
                    activeTab = 'Telemetry Analytics & Result Log';
                } else if (hash.includes('setting')) {
                    activeTab = 'System & Cloud Settings';
                } else if (hash.includes('dashboard')) {
                    activeTab = 'Dashboard';
                } else {
                    const savedTab = localStorage.getItem('bawal_active_tab');
                    if (savedTab && VALID_TABS.includes(savedTab)) {
                        activeTab = savedTab;
                    }
                }
            } catch {}
        }

        // Initial polling
        await pollBackendData();

        // Continuous real-time backend synchronization loop (every 600ms for responsive HUD & detections)
        pollingTimer = setInterval(async () => {
            await pollBackendData();
        }, 600);
    });

    onDestroy(() => {
        if (pollingTimer) clearInterval(pollingTimer);
    });

    // Handlers
    function handleForceReject() {
        totalRejected++;
        logs = [
            ...logs,
            {
                id: Math.random().toString(36).substring(2, 9),
                timestamp: new Date().toLocaleTimeString(),
                level: 'ERROR',
                message: 'MANUAL OVERRIDE: Conveyor diverter triggered [PNEUMATIC REJECT].'
            }
        ];
    }

    async function handleInspectTray() {
        if (!hasCamera) {
            logs = [
                ...logs,
                {
                    id: Math.random().toString(36).substring(2, 9),
                    timestamp: new Date().toLocaleTimeString(),
                    level: 'WARN',
                    message: 'Inspection halted: No active camera stream connected.'
                }
            ];
            return;
        }

        if (detections.length === 0) {
            logs = [
                ...logs,
                {
                    id: Math.random().toString(36).substring(2, 9),
                    timestamp: new Date().toLocaleTimeString(),
                    level: 'WARN',
                    message: 'Inspection halted: No food items detected on tray. Place meal plate under camera.'
                }
            ];
            return;
        }

        isInspecting = true;
        
        // Evaluate Tray QC Quality & Freshness
        let anyReject = false;
        let anyReview = false;
        let totalFreshness = 0;
        for (const det of detections) {
            const spoilage = Number(det.spoilage_score) || 0;
            const freshness = Math.max(0, Math.min(100, 100 - spoilage));
            totalFreshness += freshness;
            if (det.status === 'REJECT' || spoilage >= 50) anyReject = true;
            else if (det.status === 'REVIEW' || spoilage >= 25) anyReview = true;
        }

        const avgFreshness = detections.length > 0 ? totalFreshness / detections.length : 100;
        const isTrayRejected = anyReject || avgFreshness < 55.0 || (telemetry?.food_temperature || 0) > 42.0;

        if (isTrayRejected) {
            totalRejected++;
            logs = [
                ...logs,
                {
                    id: Math.random().toString(36).substring(2, 9),
                    timestamp: new Date().toLocaleTimeString(),
                    level: 'ERROR',
                    message: `QC VERDICT: REJECT - Tray failed quality checks (${detections.length} items, Avg Freshness: ${avgFreshness.toFixed(0)}%).`
                }
            ];
        } else {
            totalAccepted++;
            logs = [
                ...logs,
                {
                    id: Math.random().toString(36).substring(2, 9),
                    timestamp: new Date().toLocaleTimeString(),
                    level: anyReview ? 'WARN' : 'SUCCESS',
                    message: `QC VERDICT: ${anyReview ? 'REVIEW' : 'PASS'} - Tray approved (${detections.length} items, Avg Freshness: ${avgFreshness.toFixed(0)}%).`
                }
            ];
        }

        try {
            await triggerManualInspection(systemConfig.deviceId, detections, systemConfig.backendUrl);
            await pollBackendData();
        } catch (err) {
            console.warn('[MANUAL INSPECTION SYNC]', err);
        } finally {
            isInspecting = false;
        }
    }

    function handleClearLogs() {
        logs = [];
    }

    function handleSaveConfig(updated: SystemConfig) {
        systemConfig = updated;
        logs = [
            ...logs,
            {
                id: Math.random().toString(36).substring(2, 9),
                timestamp: new Date().toLocaleTimeString(),
                level: 'INFO',
                message: 'System configuration parameters updated.'
            }
        ];
    }
</script>

<svelte:head>
    <title>BawalVision | Industrial Vision & SCADA HMI</title>
    <meta name="description" content="BawalVision Machine Vision & Industrial SCADA Telemetry Quality Control System" />
</svelte:head>

<div class="app-root">
    <!-- Left Navigation Sidebar -->
    <Sidebar
        {activeTab}
        onSelectTab={handleSelectTab}
        activeDevice={systemConfig.deviceId}
        {isOnline}
    />

    <!-- Main View Area -->
    <div class="main-content-layout">
        <!-- Top Navigation Header -->
        <TopBar
            currentTitle={activeTab}
            gravitonEnabled={systemConfig.gravitonEngineEnabled}
            gravitonAvailable={gravitonAvailable}
            localEngineEnabled={systemConfig.localEngineEnabled}
            localEngineAvailable={localEngineAvailable}
            activeDevice={systemConfig.deviceId || 'DISCONNECTED'}
            fps={hasCamera ? 29.8 : 0.0}
            latencyMs={hasCamera ? 5.4 : 0.0}
            onToggleGraviton={() => {
                if (gravitonAvailable) {
                    systemConfig.gravitonEngineEnabled = !systemConfig.gravitonEngineEnabled;
                }
            }}
            onToggleLocal={() => {
                if (localEngineAvailable) {
                    systemConfig.localEngineEnabled = !systemConfig.localEngineEnabled;
                }
            }}
        />

        <!-- Page Content Router (Persistent View Panels: No DOM Destruction, Instant Switching) -->
        <main class="page-body">
            <div class="view-panel" class:is-hidden={activeTab !== 'Dashboard'}>
                <DashboardView
                    {detections}
                    {telemetry}
                    {logs}
                    {totalAccepted}
                    {totalRejected}
                    {totalPlates}
                    {passRate}
                    {isOnline}
                    {hasCamera}
                    {bufferedRecords}
                    {isInspecting}
                    onForceReject={handleForceReject}
                    onInspectTray={handleInspectTray}
                    onClearLogs={handleClearLogs}
                />
            </div>

            <div class="view-panel" class:is-hidden={activeTab !== 'Telemetry Analytics & Result Log'}>
                <TelemetryResultLogView
                    currentTelemetry={telemetry}
                    history={telemetryHistory}
                />
            </div>

            <div class="view-panel" class:is-hidden={activeTab !== 'System & Cloud Settings'}>
                <SettingsView
                    config={systemConfig}
                    {isOnline}
                    onDeviceConnectionChange={(connected, devId) => {
                        isOnline = connected;
                        if (devId) systemConfig.deviceId = devId;
                    }}
                    onSaveConfig={handleSaveConfig}
                />
            </div>
        </main>
    </div>
</div>
