<script lang="ts">
    import { onMount, onDestroy } from 'svelte';
    import Sidebar from '$lib/components/Sidebar.svelte';
    import TopBar from '$lib/components/TopBar.svelte';
    import OverviewView from '$lib/views/OverviewView.svelte';
    import DashboardView from '$lib/views/DashboardView.svelte';
    import InspectionReportsView from '$lib/views/InspectionReportsView.svelte';
    import BatchInspectionView from '$lib/components/BatchInspectionView.svelte';
    import TelemetryResultLogView from '$lib/views/TelemetryResultLogView.svelte';
    import GrafanaDashboardView from '$lib/views/GrafanaDashboardView.svelte';
    import SettingsView from '$lib/views/SettingsView.svelte';

    import {
        fetchRecords,
        fetchCameraStatus,
        fetchLatestTelemetry,
        fetchLatestInspection,
        fetchEngineStatus,
        triggerManualInspection,
        fetchConfidenceThreshold,
        updateConfidenceThreshold
    } from '$lib/api';

    import type {
        FoodDetection,
        TelemetryPayload,
        InferenceLogEntry,
        SystemConfig,
        SceneOpticalMetrics,
        InspectionRecord
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
        'Overview',
        'Live Inspection',
        'Inspection Reports',
        'Batch Inspection',
        'Telemetry & Hardware',
        'Grafana Dashboard',
        'Settings'
    ];

    const TAB_SLUGS: Record<string, string> = {
        'Overview': 'overview',
        'Live Inspection': 'live',
        'Inspection Reports': 'reports',
        'Batch Inspection': 'batch',
        'Telemetry & Hardware': 'telemetry',
        'Grafana Dashboard': 'grafana',
        'Settings': 'settings'
    };

    let activeTab = $state('Overview');
    let transitionDirection = $state<'forward' | 'backward' | 'none'>('none');
    let isHydrated = $state(false);
    let records = $state<InspectionRecord[]>([]);

    function setTabWithTransition(tab: string) {
        if (!VALID_TABS.includes(tab) || tab === activeTab) return;
        const prevIdx = VALID_TABS.indexOf(activeTab);
        const nextIdx = VALID_TABS.indexOf(tab);
        transitionDirection = isHydrated ? (nextIdx >= prevIdx ? 'forward' : 'backward') : 'none';
        activeTab = tab;
    }

    function handleSelectTab(tab: string) {
        if (!VALID_TABS.includes(tab)) return;
        setTabWithTransition(tab);
        if (typeof window !== 'undefined') {
            try {
                localStorage.setItem('bawal_active_tab', tab);
                window.location.hash = TAB_SLUGS[tab] ?? 'live';
            } catch {}
        }
    }
    let isOnline = $state(false);
    let hasCamera = $state(false);
    let isInspecting = $state(false);
    let gravitonAvailable = $state(false);
    let localEngineAvailable = $state(true);
    let realFps = $state(0.0);
    let realLatencyMs = $state(0.0);
    let serverMemoryMB = $state<number | null>(null);
    let parallelCores = $state(0);

    let systemConfig = $state<SystemConfig>({
        backendUrl: '/api/v1',
        deviceId: '',
        streamIntervalMs: 1500,
        enableMockIfOffline: false,
        minConfidenceThreshold: 0.35,
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
        temperature: null,
        cpu_temperature: null,
        cpu_load: null,
        timestamp: Math.floor(Date.now() / 1000),
        is_connected: false
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
    let latestOptical = $state<SceneOpticalMetrics | undefined>(undefined);

    let pollingTimer: any = null;
    let lastSlowPoll = 0;
    let isPolling = false;
    let isDestroyed = false;

    // Real Backend Polling Step
    let lastLiveDetectionTime = 0;

    function applyDetections(rawList: any[]) {
        if (!Array.isArray(rawList)) return;
        const activeThreshold = Math.min(0.98, Math.max(0.25, Number(systemConfig.minConfidenceThreshold) || 0.35));
        const filtered = rawList.filter((det: any) => {
            const conf = Number(det.similarity_rate ?? det.confidence) || 0.0;
            return conf >= activeThreshold;
        });
        if (filtered.length > 0) {
            lastLiveDetectionTime = Date.now();
            detections = filtered.map((det: any) => {
                const spoilage = Number(det.spoilage_score) || 0;
                const conf = Number(det.similarity_rate ?? det.confidence) || 0.0;
                let st: 'PASS' | 'REVIEW' | 'REJECT' = det.status;
                if (!st || (st as string) === '') {
                    st = (spoilage >= 50 || conf < activeThreshold) ? 'REJECT' : ((spoilage >= 25 || conf < (activeThreshold + 0.10)) ? 'REVIEW' : 'PASS');
                }
                const name = det.food_name || det.class_name || 'food_item';
                const box = det.box || det.bbox || [];
                return {
                    food_name: name,
                    similarity_rate: conf,
                    spoilage_score: spoilage,
                    status: st,
                    category: getCategory(name),
                    box: box
                };
            });
        }
    }

    async function pollBackendData() {
        if (isPolling) return;
        isPolling = true;
        const pollStart = typeof performance !== 'undefined' ? performance.now() : Date.now();
        const nowMs = typeof performance !== 'undefined' ? performance.now() : Date.now();
        const shouldRunSlowPoll = (nowMs - lastSlowPoll) >= 3000;
        if (shouldRunSlowPoll) {
            lastSlowPoll = nowMs;
        }

        try {
            // Parallel fetch: engine status, camera status, telemetry, and live inspections
            const [engines, cam, tel, latestLive, allRecords] = await Promise.all([
                shouldRunSlowPoll ? fetchEngineStatus(systemConfig.backendUrl) : Promise.resolve(null),
                fetchCameraStatus(systemConfig.backendUrl),
                fetchLatestTelemetry(systemConfig.backendUrl),
                fetchLatestInspection(systemConfig.backendUrl),
                shouldRunSlowPoll ? fetchRecords(systemConfig.backendUrl) : Promise.resolve(null)
            ]);

            // 1. Hardware Engine Status
            if (engines) {
                gravitonAvailable = engines.graviton_available;
                localEngineAvailable = engines.local_engine_available;
                systemConfig.gravitonEngineAvailable = engines.graviton_available;
                systemConfig.localEngineAvailable = engines.local_engine_available;
                if (engines.memory_usage_mb != null && engines.memory_usage_mb > 0) {
                    serverMemoryMB = engines.memory_usage_mb;
                } else if (engines.server_mem_sys_mb != null && engines.server_mem_sys_mb > 0) {
                    serverMemoryMB = engines.server_mem_sys_mb;
                } else if (engines.server_mem_alloc_mb != null && engines.server_mem_alloc_mb > 0) {
                    serverMemoryMB = engines.server_mem_alloc_mb;
                }
                if (engines.parallel_cores != null && engines.parallel_cores > 0) {
                    parallelCores = engines.parallel_cores;
                } else if (engines.num_cpu != null && engines.num_cpu > 0) {
                    parallelCores = engines.num_cpu;
                }
                if (!gravitonAvailable && systemConfig.gravitonEngineEnabled) {
                    systemConfig.gravitonEngineEnabled = false;
                }
            }

            // 2. Camera Status
            hasCamera = cam.active;
            realFps = cam.active ? cam.fps : 0.0;

            // 3. Telemetry Ingress from ESP32-S3
            if (tel) {
                telemetry = {
                    ...tel,
                    device_id: (cam.active && cam.deviceId && (!tel.device_id || tel.device_id === 'DISCONNECTED')) ? cam.deviceId : tel.device_id,
                    is_connected: Boolean(tel.is_connected) || cam.active
                };
                isOnline = Boolean(telemetry.is_connected);
                if (telemetry.is_connected && telemetry.temperature != null && !telemetryHistory.some(h => h.timestamp === telemetry.timestamp)) {
                    telemetryHistory = [...telemetryHistory.slice(-19), telemetry];
                }
                if (tel.scene_optical) {
                    latestOptical = tel.scene_optical;
                }
            } else {
                isOnline = cam.active;
                telemetry = {
                    ...telemetry,
                    device_id: cam.active && cam.deviceId ? cam.deviceId : 'DISCONNECTED',
                    temperature: null,
                    cpu_temperature: null,
                    cpu_load: null,
                    is_connected: false
                };
            }

            // 4. Live Real-time Inspections & Dynamic Bounding Boxes
            let hasLiveDetections = false;
            const isLiveStreamActive = Boolean(latestLive?.is_live) || cam.active;
            if (latestLive && latestLive.scene_optical) {
                latestOptical = latestLive.scene_optical;
            }
            if (latestLive && Array.isArray(latestLive.detections) && latestLive.detections.length > 0) {
                if (latestLive.is_live && (cam.active || (telemetry && telemetry.is_connected))) {
                    isOnline = true;
                }
                hasLiveDetections = true;
                applyDetections(latestLive.detections);
            } else if (isLiveStreamActive && (Date.now() - lastLiveDetectionTime > 4000)) {
                detections = [];
            }

            // 5. Inspections & Records from DB
            if (allRecords) {
                const seenKeys = new Set<string>();
                const dedupedRecords = (allRecords || []).filter(r => {
                    if (!r.inspection_foods || r.inspection_foods.length === 0) return false;
                    const key = r.video_uuid || r.id;
                    if (seenKeys.has(key)) return false;
                    seenKeys.add(key);
                    return true;
                });
                if (dedupedRecords.length > 0) {
                    records = dedupedRecords;
                }
                const validRecords = dedupedRecords;

                if (validRecords.length > 0) {
                    const spoiled = validRecords.filter(r => r.is_spoiled || (r.inspection_foods && r.inspection_foods.some(f => f.status === 'REJECT'))).length;
                    const accepted = validRecords.length - spoiled;
                    totalRejected = spoiled;
                    totalAccepted = accepted;

                    if (!hasLiveDetections && !isLiveStreamActive) {
                        const latest = validRecords[0];
                        if (latest && latest.inspection_foods && latest.inspection_foods.length > 0) {
                            detections = latest.inspection_foods.map(item => {
                                const name = item.food_item?.name || 'food_item';
                                const sp = Number(item.spoilage_score) || 0;
                                const cf = Number(item.similarity_rate) || 0.0;
                                let st: 'PASS' | 'REVIEW' | 'REJECT' = (item.status as any);
                                if (!st || (st as string) === '') {
                                    st = (sp >= 50 || cf < 0.35) ? 'REJECT' : ((sp >= 25 || cf < 0.55) ? 'REVIEW' : 'PASS');
                                }
                                return {
                                    food_name: name,
                                    similarity_rate: cf,
                                    spoilage_score: sp,
                                    status: st,
                                    category: getCategory(name)
                                };
                            });
                        } else {
                            detections = [];
                        }
                    }
                } else if (!hasLiveDetections && !isLiveStreamActive) {
                    detections = [];
                }
            }
        } catch (err) {
            console.warn('[BACKEND POLL] Offline or waiting for backend service:', err);
        } finally {
            const pollEnd = typeof performance !== 'undefined' ? performance.now() : Date.now();
            realLatencyMs = Math.max(0.5, Math.round((pollEnd - pollStart) * 10) / 10);
            isPolling = false;
        }
    }

    function syncTabFromLocation() {
        if (typeof window === 'undefined') return;
        try {
            const hash = window.location.hash.toLowerCase();
            let targetTab = 'Overview';
            if (hash.includes('batch')) {
                targetTab = 'Batch Inspection';
            } else if (hash.includes('report') || hash.includes('audit')) {
                targetTab = 'Inspection Reports';
            } else if (hash.includes('grafana')) {
                targetTab = 'Grafana Dashboard';
            } else if (hash.includes('telemetry') || hash.includes('hardware')) {
                targetTab = 'Telemetry & Hardware';
            } else if (hash.includes('setting')) {
                targetTab = 'Settings';
            } else if (hash.includes('live') || hash.includes('inspect') || hash.includes('dashboard')) {
                targetTab = 'Live Inspection';
            } else if (hash.includes('overview')) {
                targetTab = 'Overview';
            }
            if (targetTab !== activeTab) {
                setTabWithTransition(targetTab);
            }
        } catch {}
    }

    onMount(async () => {
        // Restore active tab from URL hash or localStorage and listen for SPA Back/Forward navigation
        syncTabFromLocation();
        // Enable CSR tab transition animations exclusively after client-side hydration (zero SSR / initial load lag)
        isHydrated = true;
        if (typeof window !== 'undefined') {
            window.addEventListener('hashchange', syncTabFromLocation);
            window.addEventListener('popstate', syncTabFromLocation);
            window.addEventListener('bawal-clear-detections', handleClearDetections);
            window.addEventListener('bawal-live-detections', handleLiveDetectionsEvent as EventListener);
        }

        // Initial sync of detection threshold from backend (or Docker env default)
        try {
            const threshRes = await fetchConfidenceThreshold(systemConfig.backendUrl);
            if (threshRes && threshRes.threshold) {
                systemConfig.minConfidenceThreshold = Math.min(0.98, Math.max(0.25, threshRes.threshold));
            }
        } catch {}

        // Controlled, non-blocking polling loop (prevents request stacking on cloud endpoints)
        async function runPollingLoop() {
            if (isDestroyed) return;
            await pollBackendData();
            if (!isDestroyed) {
                pollingTimer = setTimeout(runPollingLoop, 1500);
            }
        }
        runPollingLoop();
    });

    function handleLiveDetectionsEvent(e: CustomEvent) {
        if (e.detail && Array.isArray(e.detail.detections)) {
            applyDetections(e.detail.detections);
        }
    }

    function handleClearDetections() {
        detections = [];
    }

    onDestroy(() => {
        isDestroyed = true;
        if (pollingTimer) clearTimeout(pollingTimer);
        if (typeof window !== 'undefined') {
            window.removeEventListener('hashchange', syncTabFromLocation);
            window.removeEventListener('popstate', syncTabFromLocation);
            window.removeEventListener('bawal-clear-detections', handleClearDetections);
            window.removeEventListener('bawal-live-detections', handleLiveDetectionsEvent as EventListener);
        }
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
        const isTrayRejected = anyReject || avgFreshness < 55.0 || (telemetry?.temperature || 0) > 42.0;

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
            const isMock = (telemetry && (telemetry as any).is_mock_active) || (typeof localStorage !== 'undefined' && localStorage.getItem('bawal_source_mode') === 'mock');
            const targetDevice = isMock ? 'MOCK_VIDEO_CAM' : (systemConfig.deviceId || telemetry.device_id || 'ESP32S3');
            const mockSrc = typeof localStorage !== 'undefined' ? localStorage.getItem('bawal_mock_selected_video') || '' : '';
            await triggerManualInspection(targetDevice, detections, systemConfig.backendUrl, isMock ? 'mock' : 'live', mockSrc);
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

    async function handleSaveConfig(updated: SystemConfig) {
        systemConfig = updated;
        try {
            await updateConfidenceThreshold(updated.minConfidenceThreshold, systemConfig.backendUrl);
        } catch {}
        logs = [
            ...logs,
            {
                id: Math.random().toString(36).substring(2, 9),
                timestamp: new Date().toLocaleTimeString(),
                level: 'INFO',
                message: `System configuration updated (Confidence threshold: ${(updated.minConfidenceThreshold * 100).toFixed(0)}%).`
            }
        ];
    }

    let resolvedActiveDevice = $derived(
        isOnline && telemetry?.device_id && telemetry.device_id !== 'DISCONNECTED' && telemetry.device_id !== 'WAITING_DEVICE'
            ? telemetry.device_id
            : systemConfig.deviceId
    );

    let isMockMode = $derived(
        Boolean(telemetry?.is_mock_active) ||
        telemetry?.source_type === 'mock_video' ||
        (resolvedActiveDevice != null && (resolvedActiveDevice === 'MOCK_VIDEO_CAM' || resolvedActiveDevice.startsWith('MOCK_')))
    );

    let realMemoryText = $derived.by(() => {
        if (serverMemoryMB != null && serverMemoryMB > 0) {
            return `Memory usage: ${serverMemoryMB >= 1024 ? (serverMemoryMB / 1024).toFixed(2) + ' GB' : Math.round(serverMemoryMB) + ' MB'}`;
        }
        if (isOnline && telemetry?.heap_free != null && telemetry.heap_free > 0) {
            return `ESP Heap: ${Math.round(telemetry.heap_free / 1024)} KB`;
        }
        return isOnline ? 'Memory: Active' : 'Memory: Offline';
    });
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
        activeDevice={resolvedActiveDevice}
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
            activeDevice={resolvedActiveDevice || 'DISCONNECTED'}
            fps={realFps}
            latencyMs={isOnline || hasCamera ? realLatencyMs : 0.0}
            memoryUsage={realMemoryText}
            memoryMB={serverMemoryMB}
            {parallelCores}
            {isOnline}
            isMock={isMockMode}
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

        <!-- Page Content Router (Persistent View Panels: No DOM Destruction, Instant Switching + CSR 60 FPS Transitions) -->
        <main class="page-body">
            <div 
                class="view-panel" 
                class:is-active={activeTab === 'Overview'} 
                class:is-hidden={activeTab !== 'Overview'}
                class:slide-forward={transitionDirection === 'forward'}
                class:slide-backward={transitionDirection === 'backward'}
            >
                <OverviewView
                    {records}
                    {telemetry}
                    {totalAccepted}
                    {totalRejected}
                    {totalPlates}
                    {passRate}
                    {isOnline}
                    {hasCamera}
                    {realFps}
                    {realLatencyMs}
                    {serverMemoryMB}
                    {parallelCores}
                    {gravitonAvailable}
                    gravitonEnabled={systemConfig.gravitonEngineEnabled}
                    {localEngineAvailable}
                    confidenceThreshold={systemConfig.minConfidenceThreshold}
                    onNavigateTab={handleSelectTab}
                />
            </div>

            <div 
                class="view-panel" 
                class:is-active={activeTab === 'Inspection Reports'} 
                class:is-hidden={activeTab !== 'Inspection Reports'}
                class:slide-forward={transitionDirection === 'forward'}
                class:slide-backward={transitionDirection === 'backward'}
            >
                <InspectionReportsView />
            </div>

            <div 
                class="view-panel" 
                class:is-active={activeTab === 'Batch Inspection'} 
                class:is-hidden={activeTab !== 'Batch Inspection'}
                class:slide-forward={transitionDirection === 'forward'}
                class:slide-backward={transitionDirection === 'backward'}
            >
                <BatchInspectionView />
            </div>

            <div 
                class="view-panel" 
                class:is-active={activeTab === 'Live Inspection'} 
                class:is-hidden={activeTab !== 'Live Inspection'}
                class:slide-forward={transitionDirection === 'forward'}
                class:slide-backward={transitionDirection === 'backward'}
            >
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
                    sceneOptical={latestOptical || telemetry?.scene_optical}
                    onForceReject={handleForceReject}
                    onInspectTray={handleInspectTray}
                    onClearLogs={handleClearLogs}
                />
            </div>

            <div 
                class="view-panel" 
                class:is-active={activeTab === 'Telemetry & Hardware'} 
                class:is-hidden={activeTab !== 'Telemetry & Hardware'}
                class:slide-forward={transitionDirection === 'forward'}
                class:slide-backward={transitionDirection === 'backward'}
            >
                <TelemetryResultLogView
                    currentTelemetry={telemetry}
                    history={telemetryHistory}
                />
            </div>

            <div 
                class="view-panel" 
                class:is-active={activeTab === 'Grafana Dashboard'} 
                class:is-hidden={activeTab !== 'Grafana Dashboard'}
                class:slide-forward={transitionDirection === 'forward'}
                class:slide-backward={transitionDirection === 'backward'}
            >
                <GrafanaDashboardView />
            </div>

            <div 
                class="view-panel" 
                class:is-active={activeTab === 'Settings'} 
                class:is-hidden={activeTab !== 'Settings'}
                class:slide-forward={transitionDirection === 'forward'}
                class:slide-backward={transitionDirection === 'backward'}
            >
                <SettingsView
                    config={systemConfig}
                    {isOnline}
                    {parallelCores}
                    onDeviceConnectionChange={(connected, devId) => {
                        isOnline = connected;
                        if (!connected) {
                            telemetry = {
                                ...telemetry,
                                temperature: null,
                                cpu_temperature: null,
                                cpu_load: null,
                                is_connected: false
                            };
                        } else if (devId) {
                            telemetry = {
                                ...telemetry,
                                device_id: devId,
                                is_connected: true
                            };
                        }
                    }}
                    onSaveConfig={handleSaveConfig}
                />
            </div>
        </main>
    </div>
</div>
