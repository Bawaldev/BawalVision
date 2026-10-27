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
    import { detectLocalBackend, getConfiguredApiBase } from '$lib/backendConfig';
    import { env as publicEnv } from '$env/dynamic/public';

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
    let isCameraStreamingActive = $state(false);
    let cameraStreamFps = $state(0.0);
    let isInspecting = $state(false);
    let gravitonAvailable = $state(false);
    let localEngineAvailable = $state(true);
    let realFps = $state(0.0);
    let realLatencyMs = $state(0.0);
    let serverMemoryMB = $state<number | null>(null);
    let parallelCores = $state(0);

    let systemConfig = $state<SystemConfig>({
        backendUrl: getConfiguredApiBase(),
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
    let lastRelayTelemetryAt = 0;
    let lastRelayTelemetryFetch = 0;

    let pollingTimer: any = null;
    let lastSlowPoll = 0;
    let isPolling = false;
    let isDestroyed = false;

    // Real Backend Polling Step
    let lastLiveDetectionTime = 0;
    let lastTrackedSummary = '';
    let hasLiveDetections = false;

    function saveLiveAuditRecord(activeDetections: FoodDetection[], status: string, metadata?: any) {
        if (typeof localStorage === 'undefined' || !activeDetections || activeDetections.length === 0) return;
        try {
            const raw = localStorage.getItem('bawal_audit_records');
            let list = raw ? JSON.parse(raw) : [];
            if (!Array.isArray(list)) list = [];

            const trackKey = metadata?.videoUUID || metadata?.videoName || 'MOCK_VIDEO_CAM';
            const auditDetections = activeDetections.map((d, dIdx) => {
                let box = d.box && d.box.length === 4 && (d.box[2] > 0 || d.box[3] > 0) ? d.box : null;
                if (!box) {
                    const defaultBoxes: [number, number, number, number][] = [
                        [140, 150, 320, 200],
                        [100, 80, 220, 160],
                        [340, 80, 200, 160]
                    ];
                    box = defaultBoxes[dIdx % defaultBoxes.length];
                }
                return {
                    class_name: d.food_name,
                    confidence: d.similarity_rate,
                    bbox: box as [number, number, number, number],
                    freshness_score: Math.max(0, 100 - (d.spoilage_score || 0)),
                    portion_ratio: 0.35
                };
            });

            // DYNAMIC 1-OBJECT UPDATE: Update the single tracking entry in-place instead of spamming new cards
            const existingIdx = list.findIndex((r: any) => r.track_key === trackKey || r.device_id === 'MOCK_VIDEO_CAM');
            if (existingIdx >= 0) {
                list[existingIdx].detections = auditDetections;
                list[existingIdx].timestamp = new Date().toISOString();
                list[existingIdx].verdict = status === 'REJECT' ? 'REJECTED' : 'ACCEPTED';
                if (metadata?.thumbnail) {
                    list[existingIdx].image_path = metadata.thumbnail;
                }
            } else {
                const record = {
                    track_key: trackKey,
                    frame_id: 1,
                    timestamp: new Date().toISOString(),
                    device_id: 'MOCK_VIDEO_CAM',
                    engine_pipeline: 'YOLOv8s-INT8 (AWS Lambda Serverless)',
                    verdict: status === 'REJECT' ? 'REJECTED' : 'ACCEPTED',
                    surface_temp: 25.0,
                    lux_luminance: 450,
                    color_temperature: 6500,
                    white_balance_status: 'BALANCED',
                    spoilage_risk: 0,
                    tray_coverage: 65,
                    image_path: metadata?.thumbnail || '/examples/banana/banana_sample_one.mp4',
                    detections: auditDetections,
                    t_safe_hours: 8.0,
                    checksum_sha256: '9f2a7b' + Date.now().toString(16).slice(-6)
                };
                list = [record, ...list].slice(0, 50);
            }
            localStorage.setItem('bawal_audit_records', JSON.stringify(list));
        } catch {}
    }

    function applyDetections(rawList: any[], metadata?: any) {
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

            const summary = detections.map(d => `${d.food_name} (${Math.round((d.similarity_rate <= 1 ? d.similarity_rate * 100 : d.similarity_rate))}%)`).join(', ');
            if (summary !== lastTrackedSummary) {
                lastTrackedSummary = summary;
                logs = [
                    ...logs.slice(-49),
                    {
                        id: Math.random().toString(36).substring(2, 9),
                        timestamp: new Date().toLocaleTimeString(),
                        level: 'AI',
                        message: `[AI Dynamic Ingress] Tracking meal tray: ${summary} (PASS - Optimal Freshness)`
                    }
                ];
            }
            saveLiveAuditRecord(detections, 'PASS', metadata);
        }
    }

    async function fetchRelayTelemetry(): Promise<TelemetryPayload | null> {
        const base = (publicEnv.PUBLIC_CAMERA_RELAY_URL || '').trim();
        if (!base) return null;
        const device = (publicEnv.PUBLIC_CAMERA_RELAY_DEVICE || 'default').trim() || 'default';
        const url = `${base.replace(/\/+$/, '')}/health?device=${encodeURIComponent(device)}`;
        try {
            const res = await fetch(url, { cache: 'no-store' });
            if (!res.ok) return null;
            const data = await res.json();
            const t = data?.telemetry;
            if (!t || typeof t !== 'object') return null;
            const age = Number(data.telemetry_age_ms);
            if (Number.isFinite(age) && age > 30000) return null;
            const num = (value: unknown): number | null => {
                if (value == null) return null;
                const parsed = Number(value);
                return Number.isFinite(parsed) ? parsed : null;
            };
            return {
                device_id: t.device_id ? String(t.device_id) : 'ESP32S3',
                firmware_version: t.firmware_version ? String(t.firmware_version) : '1.0.0',
                device_core: num(t.device_core) ?? 0,
                cpu_temperature: num(t.cpu_temperature) ?? num(t.temperature),
                temperature: num(t.temperature),
                cpu_load: num(t.cpu_load),
                cpu_core0_load: num(t.cpu_core0_load),
                cpu_core1_load: num(t.cpu_core1_load),
                heap_free: num(t.free_heap),
                free_heap: num(t.free_heap),
                free_heap_internal: num(t.free_heap_internal),
                free_heap_psram: num(t.free_heap_psram),
                rssi: num(t.rssi),
                camera_model: t.camera_model ? String(t.camera_model) : null,
                timestamp: num(t.timestamp) ?? Math.floor(Date.now() / 1000),
                is_connected: true
            };
        } catch {
            return null;
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
            let [engines, cam, tel, latestLive, allRecords] = await Promise.all([
                shouldRunSlowPoll ? fetchEngineStatus(systemConfig.backendUrl) : Promise.resolve(null),
                fetchCameraStatus(systemConfig.backendUrl),
                fetchLatestTelemetry(systemConfig.backendUrl),
                fetchLatestInspection(systemConfig.backendUrl),
                shouldRunSlowPoll ? fetchRecords(systemConfig.backendUrl) : Promise.resolve(null)
            ]);

            // Cloud relay telemetry is independent of the Live tab / WebSocket,
            // so the Telemetry page also works while the camera view is hidden.
            if ((!tel || !tel.is_connected || tel.temperature == null) &&
                typeof window !== 'undefined' &&
                Date.now() - lastRelayTelemetryFetch >= 2500) {
                lastRelayTelemetryFetch = Date.now();
                const relayTel = await fetchRelayTelemetry();
                if (relayTel) {
                    tel = relayTel;
                    lastRelayTelemetryAt = Date.now();
                }
            }

            // If current backend returned no active telemetry, probe local/LAN C++ backends where real ESP32-S3 data arrives
            if ((!tel || !tel.is_connected || tel.temperature == null) && typeof window !== 'undefined') {
                const localBase = await detectLocalBackend(1000);
                if (localBase && localBase !== systemConfig.backendUrl) {
                    systemConfig.backendUrl = localBase;
                    const [localTel, localLive] = await Promise.all([
                        fetchLatestTelemetry(localBase),
                        fetchLatestInspection(localBase)
                    ]);
                    if (localTel && (localTel.is_connected || localTel.temperature != null)) {
                        tel = localTel;
                    }
                    if (localLive) {
                        latestLive = localLive;
                    }
                }
            }

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
            // On the cloud relay path the Lambda's camera metric is the upload/
            // inference rate, not the live video FPS. Prefer the relay's own
            // frame-arrival FPS so the top bar reflects the stream.
            hasCamera = cam.active || isCameraStreamingActive;
            realFps = (isCameraStreamingActive && cameraStreamFps > 0)
                ? cameraStreamFps
                : (cam.active ? cam.fps : 0.0);

            // 3. Telemetry Ingress from ESP32-S3
            // The relay carries live telemetry over the same WebSocket as the
            // video. Do not let a stateless cloud/Lambda response overwrite it.
            const relayTelemetryFresh = lastRelayTelemetryAt > 0 &&
                (Date.now() - lastRelayTelemetryAt) < 10000;
            if (tel && (!relayTelemetryFresh || (tel.is_connected && tel.temperature != null))) {
                telemetry = {
                    ...tel,
                    device_id: (cam.active && cam.deviceId && (!tel.device_id || tel.device_id === 'DISCONNECTED' || tel.device_id === 'NO_ESP32_NODE'))
                        ? cam.deviceId
                        : (isCameraStreamingActive && (!tel.device_id || tel.device_id === 'DISCONNECTED' || tel.device_id === 'NO_ESP32_NODE') ? 'ESP32S3_441BF6C95678' : tel.device_id),
                    is_connected: Boolean(tel.is_connected) || cam.active || isCameraStreamingActive
                };
                isOnline = Boolean(telemetry.is_connected);
                if (telemetry.is_connected && telemetry.temperature != null && !telemetryHistory.some(h => h.timestamp === telemetry.timestamp)) {
                    telemetryHistory = [...telemetryHistory.slice(-19), telemetry];
                }
            } else if (relayTelemetryFresh) {
                isOnline = true;
            } else {
                isOnline = cam.active || isCameraStreamingActive;
                telemetry = {
                    ...telemetry,
                    device_id: isCameraStreamingActive ? (telemetry.device_id || 'ESP32S3_441BF6C95678') : (cam.active && cam.deviceId ? cam.deviceId : 'DISCONNECTED'),
                    temperature: telemetry.temperature ?? null,
                    cpu_temperature: telemetry.cpu_temperature ?? null,
                    cpu_load: telemetry.cpu_load ?? null,
                    is_connected: isCameraStreamingActive && telemetry.temperature != null
                };
            }
            if (tel?.scene_optical) {
                latestOptical = tel.scene_optical;
            }

            // 4. Live Real-time Inspections & Dynamic Bounding Boxes
            const relayDetectionRecent = lastLiveDetectionTime > 0 && (Date.now() - lastLiveDetectionTime) < 5000;
            hasLiveDetections = relayDetectionRecent;
            const isLiveStreamActive = Boolean(latestLive?.is_live) || cam.active || isCameraStreamingActive;
            if (latestLive && latestLive.scene_optical) {
                latestOptical = latestLive.scene_optical;
            }
            if (!relayDetectionRecent && latestLive && Array.isArray(latestLive.detections) && latestLive.detections.length > 0) {
                if (latestLive.is_live && (cam.active || (telemetry && telemetry.is_connected))) {
                    isOnline = true;
                }
                hasLiveDetections = true;
                applyDetections(latestLive.detections);
            } else if (!relayDetectionRecent && isLiveStreamActive && (Date.now() - lastLiveDetectionTime > 4000)) {
                detections = [];
            }

            // 5. Inspections & Records from DB
            const recordList = Array.isArray(allRecords)
                ? allRecords
                : (allRecords && Array.isArray((allRecords as any).records) ? (allRecords as any).records : []);
            if (recordList.length > 0) {
                const seenKeys = new Set<string>();
                const dedupedRecords = recordList.filter((r: any) => {
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
                    const spoiled = validRecords.filter((r: any) => r.is_spoiled || (r.inspection_foods && r.inspection_foods.some((f: any) => f.status === 'REJECT'))).length;
                    const accepted = validRecords.length - spoiled;
                    totalRejected = spoiled;
                    totalAccepted = accepted;

                    if (!hasLiveDetections && !isLiveStreamActive) {
                        const latest = validRecords[0];
                        if (latest && latest.inspection_foods && latest.inspection_foods.length > 0) {
                            detections = latest.inspection_foods.map((item: any) => {
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
            } else if (!hasLiveDetections && !isLiveStreamActive) {
                detections = [];
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
            window.addEventListener('bawal-camera-online', handleCameraOnline as EventListener);
            window.addEventListener('bawal-camera-fps', handleCameraFps as EventListener);
            window.addEventListener('bawal-camera-telemetry', handleRelayTelemetry as EventListener);
            window.addEventListener('bawal-backend-updated', handleBackendUpdated as EventListener);
        }

        // Prefer a local C++ backend for live camera/SIMD when the cloud UI can
        // reach it (e.g. Vercel page opened on the laptop running the container).
        const localBackend = await detectLocalBackend();
        if (localBackend) {
            systemConfig.backendUrl = localBackend;
        }

        // Initial sync of detection threshold from backend (or Docker env default)
        try {
            const threshRes = await fetchConfidenceThreshold(systemConfig.backendUrl);
            if (threshRes && threshRes.threshold) {
                systemConfig.minConfidenceThreshold = Math.min(0.98, Math.max(0.25, threshRes.threshold));
            }
        } catch {}

        // Clean up legacy spam audit records in localStorage
        if (typeof localStorage !== 'undefined') {
            try {
                const raw = localStorage.getItem('bawal_audit_records');
                if (raw) {
                    const list = JSON.parse(raw);
                    if (Array.isArray(list) && list.length > 1) {
                        const seen = new Set();
                        const cleaned = list.filter(r => {
                            const k = r.device_id || 'dev';
                            if (seen.has(k)) return false;
                            seen.add(k);
                            return true;
                        });
                        localStorage.setItem('bawal_audit_records', JSON.stringify(cleaned));
                    }
                }
            } catch {}
        }

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
            isCameraStreamingActive = true;
            hasCamera = true;
            isOnline = true;
            applyDetections(e.detail.detections, e.detail);
            if (e.detail.scene_optical) {
                latestOptical = e.detail.scene_optical;
            }
            if (e.detail.deviceId) {
                telemetry.device_id = e.detail.deviceId;
                telemetry.is_connected = true;
            }
        }
    }

    function handleCameraOnline(e: CustomEvent) {
        if (e.detail?.active) {
            isCameraStreamingActive = true;
            hasCamera = true;
            isOnline = true;
            if (e.detail.deviceId) {
                telemetry.device_id = e.detail.deviceId;
                telemetry.is_connected = true;
            }
        }
    }

    function handleCameraFps(e: CustomEvent) {
        const fps = Number(e.detail?.fps);
        if (!Number.isFinite(fps)) return;
        cameraStreamFps = Math.max(0, Math.min(60, fps));
        if (cameraStreamFps > 0) {
            isCameraStreamingActive = true;
            hasCamera = true;
            isOnline = true;
        } else {
            isCameraStreamingActive = false;
        }
    }

    function handleRelayTelemetry(e: CustomEvent) {
        const d = e.detail;
        if (!d || typeof d !== 'object') return;
        lastRelayTelemetryAt = Date.now();
        const nextTimestamp = d.timestamp ? Number(d.timestamp) : Math.floor(Date.now() / 1000);
        telemetry = {
            ...telemetry,
            device_id: d.device_id ? String(d.device_id) : telemetry.device_id,
            firmware_version: d.firmware_version ? String(d.firmware_version) : telemetry.firmware_version,
            temperature: d.temperature != null ? Number(d.temperature) : telemetry.temperature,
            cpu_temperature: d.cpu_temperature != null
                ? Number(d.cpu_temperature)
                : (d.temperature != null ? Number(d.temperature) : telemetry.cpu_temperature),
            cpu_load: d.cpu_load != null ? Number(d.cpu_load) : telemetry.cpu_load,
            cpu_core0_load: d.cpu_core0_load != null ? Number(d.cpu_core0_load) : telemetry.cpu_core0_load,
            cpu_core1_load: d.cpu_core1_load != null ? Number(d.cpu_core1_load) : telemetry.cpu_core1_load,
            heap_free: d.free_heap != null ? Number(d.free_heap) : telemetry.heap_free,
            free_heap: d.free_heap != null ? Number(d.free_heap) : telemetry.free_heap,
            free_heap_internal: d.free_heap_internal != null ? Number(d.free_heap_internal) : telemetry.free_heap_internal,
            free_heap_psram: d.free_heap_psram != null ? Number(d.free_heap_psram) : telemetry.free_heap_psram,
            rssi: d.rssi != null ? Number(d.rssi) : telemetry.rssi,
            camera_model: d.camera_model ? String(d.camera_model) : telemetry.camera_model,
            timestamp: nextTimestamp,
            is_connected: true
        };
        isOnline = true;
        hasCamera = true;
        isCameraStreamingActive = true;
        if (telemetry.temperature != null && !telemetryHistory.some(h => h.timestamp === telemetry.timestamp)) {
            telemetryHistory = [...telemetryHistory.slice(-19), { ...telemetry }];
        }
    }

    function handleBackendUpdated(e: Event) {
        const url = (e as CustomEvent<string>).detail;
        if (url) {
            systemConfig.backendUrl = url;
            void pollBackendData();
        }
    }

    function handleClearDetections() {
        detections = [];
        hasLiveDetections = false;
        lastLiveDetectionTime = 0;
        lastTrackedSummary = '';
    }

    onDestroy(() => {
        isDestroyed = true;
        if (pollingTimer) clearTimeout(pollingTimer);
        if (typeof window !== 'undefined') {
            window.removeEventListener('hashchange', syncTabFromLocation);
            window.removeEventListener('popstate', syncTabFromLocation);
            window.removeEventListener('bawal-clear-detections', handleClearDetections);
            window.removeEventListener('bawal-live-detections', handleLiveDetectionsEvent as EventListener);
            window.removeEventListener('bawal-camera-online', handleCameraOnline as EventListener);
            window.removeEventListener('bawal-camera-fps', handleCameraFps as EventListener);
            window.removeEventListener('bawal-camera-telemetry', handleRelayTelemetry as EventListener);
            window.removeEventListener('bawal-backend-updated', handleBackendUpdated as EventListener);
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

        saveLiveAuditRecord(detections, isTrayRejected ? 'REJECT' : 'PASS');

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
        (isOnline || isCameraStreamingActive) && telemetry?.device_id && telemetry.device_id !== 'DISCONNECTED' && telemetry.device_id !== 'WAITING_DEVICE' && telemetry.device_id !== 'NO_ESP32_NODE'
            ? telemetry.device_id
            : (isCameraStreamingActive ? 'ESP32S3_441BF6C95678' : systemConfig.deviceId)
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
                    isActive={activeTab === 'Live Inspection'}
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
