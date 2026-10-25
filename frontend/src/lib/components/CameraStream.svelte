<script lang="ts">
    import { onMount, onDestroy } from 'svelte';
    import type { FoodDetection, SceneOpticalMetrics } from '../types';

    interface Props {
        detections: FoodDetection[];
        deviceId?: string;
        isLive?: boolean;
        hasCamera?: boolean;
        streamUrl?: string;
        sceneOptical?: SceneOpticalMetrics;
    }

    let {
        detections = [],
        deviceId = '',
        isLive = true,
        hasCamera = false,
        streamUrl = '',
        sceneOptical = undefined
    }: Props = $props();

    const standardFoodItems = [
        { id: 'rice', aliases: ['rice', 'nasi'], label: 'Rice (Nasi)', icon: '🍚' },
        { id: 'chicken', aliases: ['chicken', 'ayam'], label: 'Chicken (Ayam)', icon: '🍗' },
        { id: 'egg', aliases: ['egg', 'telur'], label: 'Egg (Telur)', icon: '🥚' },
        { id: 'tempeh', aliases: ['tempeh', 'tempe'], label: 'Tempeh (Tempe)', icon: '🧇' },
        { id: 'tofu', aliases: ['tofu', 'tahu'], label: 'Tofu (Tahu)', icon: '🧊' },
        { id: 'banana', aliases: ['banana', 'pisang'], label: 'Banana (Pisang)', icon: '🍌' },
        { id: 'orange', aliases: ['orange', 'jeruk'], label: 'Orange (Jeruk)', icon: '🍊' },
        { id: 'apple', aliases: ['apple', 'apel'], label: 'Apple (Apel)', icon: '🍎' },
        { id: 'spinach', aliases: ['spinach', 'bayam'], label: 'Spinach (Bayam)', icon: '🥬' },
        { id: 'water_spinach', aliases: ['water_spinach', 'waterspinach', 'kangkung'], label: 'Water Spinach (Kangkung)', icon: '🌱' }
    ];

    type SourceModeType = 'live' | 'mock';
    let sourceMode = $state<SourceModeType>('live');

    // Deterministic RFC-4122 UUID v5 generator matching backend inspection.DeterministicMockVideoUUID byte-for-byte
    function computeMockVideoUUID(videoKey: string): string {
        let cleaned = (videoKey || '').trim().toLowerCase();
        if (cleaned.startsWith('/api/v1/examples/file/')) {
            cleaned = cleaned.slice('/api/v1/examples/file/'.length);
        }
        if (cleaned.startsWith('examples/')) {
            cleaned = cleaned.slice('examples/'.length);
        }
        if (!cleaned) {
            cleaned = 'default_mock_video';
        }
        if (/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(cleaned)) {
            return cleaned.toLowerCase();
        }
        const input = 'bawalvision:mock-video:' + cleaned;
        const seeds = [0x811c9dc5, 0x9e3779b9, 0x85ebca6b, 0xc2b2ae35];
        const b = new Uint8Array(16);
        for (let sIdx = 0; sIdx < 4; sIdx++) {
            let h = seeds[sIdx] >>> 0;
            for (let i = 0; i < input.length; i++) {
                h = (h ^ input.charCodeAt(i)) >>> 0;
                h = Math.imul(h, 0x01000193) >>> 0;
            }
            h = (h ^ Math.imul(sIdx, 0x27d4eb2d)) >>> 0;
            h = (h ^ (h >>> 16)) >>> 0;
            h = Math.imul(h, 0x85ebca6b) >>> 0;
            h = (h ^ (h >>> 13)) >>> 0;
            h = Math.imul(h, 0xc2b2ae35) >>> 0;
            h = (h ^ (h >>> 16)) >>> 0;

            const offset = sIdx * 4;
            b[offset] = (h >>> 24) & 0xff;
            b[offset + 1] = (h >>> 16) & 0xff;
            b[offset + 2] = (h >>> 8) & 0xff;
            b[offset + 3] = h & 0xff;
        }
        b[6] = (b[6] & 0x0f) | 0x50;
        b[8] = (b[8] & 0x3f) | 0x80;

        const hex = Array.from(b, (byte) => byte.toString(16).padStart(2, '0')).join('');
        return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20, 32)}`;
    }

    let exampleVideos = $state<string[]>([
        'banana/banana_sample_one.mp4'
    ]);
    let videoUUIDMap = $state<Record<string, string>>({});
    let selectedVideo = $state('banana/banana_sample_one.mp4');
    let mockVideoSrc = $state('/examples/banana/banana_sample_one.mp4');
    let mockFileName = $state('banana/banana_sample_one.mp4');
    let customFileKey = $state('');
    let isCustomUpload = $state(false);
    let fileInputRef = $state<HTMLInputElement | null>(null);

    let mockVideoUUID = $derived(
        isCustomUpload
            ? computeMockVideoUUID(customFileKey || `custom:${mockFileName}`)
            : (videoUUIDMap[mockFileName] || computeMockVideoUUID(mockFileName))
    );

    // Mock Video AI Frame Ingestion Pipeline State & Hardware Handlers
    let mockVideoEl = $state<HTMLVideoElement | null>(null);
    let mockCanvasEl = $state<HTMLCanvasElement | null>(null);
    let mockIsPlaying = $state(false);
    let mockCurrentTime = $state(0);
    let mockDuration = $state(0);
    let mockPlaybackRate = $state(1.0);
    let mockPushedFrames = $state(0);
    let isPushingFrame = false;
    let pushInterval: any = null;
    let liveRefreshTimer: any = null;
    let mockFrameSeq = 0;
    let isRewound = $state(false);
    let lastVideoTime = 0;

    let streamError = $state(false);
    let streamEpoch = $state(Date.now());
    let discoveredStreamUrl = $state('');
    let deviceStatusTimer: ReturnType<typeof setInterval> | null = null;
    let retryCount = $state(0);
    const MAX_RETRIES = 3;
    let isTabVisible = $state(true);

    let activeStreamUrl = $derived(
        !isTabVisible
            ? ''
            : (streamUrl || discoveredStreamUrl || `/api/v1/camera/stream?t=${streamEpoch}`)
    );

    function handleVisibilityChange() {
        if (typeof document !== 'undefined') {
            if (document.hidden) {
                isTabVisible = false;
                if (liveRefreshTimer !== null) {
                    clearTimeout(liveRefreshTimer);
                    liveRefreshTimer = null;
                }
            } else {
                streamEpoch = Date.now();
                isTabVisible = true;
            }
        }
    }

    function scheduleLiveRefresh() {
        if (streamUrl || discoveredStreamUrl || sourceMode !== 'live' || !isTabVisible) return;
        if (liveRefreshTimer !== null) clearTimeout(liveRefreshTimer);
        // The Crow route sends one snapshot and closes the response. Re-arm
        // only after that image loaded so slow responses never pile up.
        liveRefreshTimer = setTimeout(() => {
            liveRefreshTimer = null;
            if (sourceMode === 'live' && isTabVisible) streamEpoch = Date.now();
        }, 100);
    }

    function handleGlobalSourceMode(event: Event) {
        const mode = (event as CustomEvent<string>).detail;
        if (mode === 'mock' || mode === 'live') {
            void setSourceMode(mode);
        }
    }

    // Initial mount: restore persisted source mode, selected video, and fetch examples list with unique UUIDs
    onMount(() => {
        window.addEventListener('bawal-source-mode', handleGlobalSourceMode);
        const refreshDiscoveredStream = () => {
            fetch('/api/v1/devices/status', { cache: 'no-store' })
                .then(res => res.ok ? res.json() : null)
                .then(data => {
                    const nextUrl = typeof data?.stream_url === 'string' ? data.stream_url : '';
                    if (nextUrl && nextUrl !== discoveredStreamUrl) {
                        discoveredStreamUrl = nextUrl;
                        streamError = false;
                    }
                })
                .catch(() => {});
        };
        refreshDiscoveredStream();
        deviceStatusTimer = setInterval(refreshDiscoveredStream, 2000);
        if (typeof document !== 'undefined') {
            document.addEventListener('visibilitychange', handleVisibilityChange);
        }
        if (typeof localStorage !== 'undefined') {
            const savedVideo = localStorage.getItem('bawal_mock_selected_video');
            if (savedVideo && savedVideo !== 'custom') {
                selectedVideo = savedVideo;
                mockFileName = savedVideo;
                mockVideoSrc = `/examples/${savedVideo}`;
            }
            const saved = localStorage.getItem('bawal_source_mode');
            if (saved === 'mock' || saved === 'live') {
                sourceMode = saved;
                if (saved === 'mock') {
                    fetch('/api/v1/camera/mode', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({
                            mode: 'mock',
                            video_source: mockFileName,
                            video_uuid: computeMockVideoUUID(mockFileName)
                        })
                    }).catch(() => {});
                }
            }
        }
        fetch('/api/v1/examples')
            .then(res => res.ok ? res.json() : null)
            .then(data => {
                if (data && data.video_uuids && typeof data.video_uuids === 'object') {
                    videoUUIDMap = data.video_uuids;
                }
                if (data && Array.isArray(data.videos) && data.videos.length > 0) {
                    exampleVideos = data.videos;
                    if (!data.videos.includes(selectedVideo) && !isCustomUpload) {
                        selectExample(data.videos[0]);
                    }
                }
            })
            .catch(() => {
                // Fallback to presets if backend is unavailable
            });

    });

    function formatExampleLabel(path: string): string {
        const parts = path.split('/');
        if (parts.length > 1) {
            const category = parts[0];
            const file = parts.slice(1).join('/');
            const iconMap: Record<string, string> = {
                banana: '🍌',
                apple: '🍎',
                orange: '🍊',
                spinach: '🥬',
                water_spinach: '🌱',
                fish: '🐟',
                meat: '🥩',
                vegetable: '🥦',
                fruit: '🍉'
            };
            const icon = iconMap[category.toLowerCase()] || '📁';
            return `${icon} [${category}] ${file}`;
        }
        return `📹 ${path}`;
    }

    function drawVideoPreservingAspect(ctx: CanvasRenderingContext2D, video: HTMLVideoElement, cw: number, ch: number) {
        const vw = video.videoWidth || cw;
        const vh = video.videoHeight || ch;
        const scale = Math.min(cw / vw, ch / vh);
        const dw = Math.round(vw * scale);
        const dh = Math.round(vh * scale);
        const dx = Math.floor((cw - dw) / 2);
        const dy = Math.floor((ch - dh) / 2);
        if (dx > 0 || dy > 0) {
            ctx.fillStyle = '#727272';
            ctx.fillRect(0, 0, cw, ch);
        }
        ctx.drawImage(video, dx, dy, dw, dh);
    }

    function captureAndPushFrame() {
        if (!mockVideoEl || !mockCanvasEl || isPushingFrame) return;
        if (mockVideoEl.paused || mockVideoEl.ended || mockVideoEl.readyState < 2) return;

        const ctx = mockCanvasEl.getContext('2d');
        if (!ctx) return;

        isPushingFrame = true;
        try {
            drawVideoPreservingAspect(ctx, mockVideoEl, mockCanvasEl.width, mockCanvasEl.height);
            mockCanvasEl.toBlob((blob) => {
                if (!blob) {
                    isPushingFrame = false;
                    return;
                }
                const currentSeq = ++mockFrameSeq;
                fetch('/api/v1/camera/frame', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'image/jpeg',
                        'X-Device-ID': 'MOCK_VIDEO_CAM',
                        'X-Stream-Source': 'MOCK_VIDEO',
                        'X-Telemetry-Mode': 'MOCK_SIMULATION',
                        'X-Telemetry-Struct': 'MockTelemetryPayload',
                        'X-Mock-Video-Source': mockFileName,
                        'X-Mock-Video-UUID': mockVideoUUID,
                        'X-Frame-ID': String(currentSeq),
                        'X-Is-Rewind': isRewound ? '1' : '0',
                        'X-Video-Rewind': isRewound ? '1' : '0'
                    },
                    body: blob
                })
                .then(async (res) => {
                    if (res.ok) {
                        mockPushedFrames++;
                        streamError = false;
                        const data = await res.json().catch(() => null);
                        if (data && Array.isArray(data.detections)) {
                            let thumb = '';
                            try {
                                if (mockCanvasEl && data.detections.length > 0) {
                                    thumb = mockCanvasEl.toDataURL('image/jpeg', 0.65);
                                }
                            } catch {}
                            window.dispatchEvent(new CustomEvent('bawal-live-detections', {
                                detail: {
                                    ...data,
                                    source: 'mock_video',
                                    videoName: mockFileName,
                                    videoUUID: mockVideoUUID,
                                    thumbnail: thumb
                                }
                            }));
                        }
                    }
                })
                .catch(() => {})
                .finally(() => {
                    isPushingFrame = false;
                });
            }, 'image/jpeg', 0.85);
        } catch {
            isPushingFrame = false;
        }
    }

    function pushCurrentFrameOnce() {
        if (!mockVideoEl || !mockCanvasEl) return;
        if (mockVideoEl.readyState < 2) return;
        const ctx = mockCanvasEl.getContext('2d');
        if (!ctx) return;
        try {
            drawVideoPreservingAspect(ctx, mockVideoEl, mockCanvasEl.width, mockCanvasEl.height);
            mockCanvasEl.toBlob((blob) => {
                if (!blob) return;
                fetch('/api/v1/camera/frame', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'image/jpeg',
                        'X-Device-ID': 'MOCK_VIDEO_CAM',
                        'X-Stream-Source': 'MOCK_VIDEO',
                        'X-Telemetry-Mode': 'MOCK_SIMULATION',
                        'X-Telemetry-Struct': 'MockTelemetryPayload',
                        'X-Mock-Video-Source': mockFileName,
                        'X-Mock-Video-UUID': mockVideoUUID,
                        'X-Frame-ID': String(++mockFrameSeq),
                        'X-Is-Rewind': isRewound ? '1' : '0',
                        'X-Video-Rewind': isRewound ? '1' : '0'
                    },
                    body: blob
                }).then(async (res) => {
                    if (res && res.ok) {
                        mockPushedFrames++;
                        streamError = false;
                        const data = await res.json().catch(() => null);
                        if (data && Array.isArray(data.detections)) {
                            let thumb = '';
                            try {
                                if (mockCanvasEl && data.detections.length > 0) {
                                    thumb = mockCanvasEl.toDataURL('image/jpeg', 0.65);
                                }
                            } catch {}
                            window.dispatchEvent(new CustomEvent('bawal-live-detections', {
                                detail: {
                                    ...data,
                                    source: 'mock_video',
                                    videoName: mockFileName,
                                    videoUUID: mockVideoUUID,
                                    thumbnail: thumb
                                }
                            }));
                        }
                    }
                }).catch(() => {});
            }, 'image/jpeg', 0.85);
        } catch {}
    }

    function startMockPushing() {
        stopMockPushing();
        // Run continuous frame capture into AI vision pipeline (optimized for network/serverless ingress)
        pushInterval = setInterval(() => {
            if (sourceMode === 'mock') {
                captureAndPushFrame();
            }
        }, 250);
    }

    function stopMockPushing() {
        if (pushInterval) {
            clearInterval(pushInterval);
            pushInterval = null;
        }
    }

    let isSwitchingMode = $state(false);

    async function setSourceMode(mode: SourceModeType) {
        if (sourceMode === mode && !isSwitchingMode) return;
        isSwitchingMode = true;
        sourceMode = mode;
        if (typeof localStorage !== 'undefined') {
            try { localStorage.setItem('bawal_source_mode', mode); } catch {}
        }
        streamError = false;
        retryCount = 0;
        if (typeof window !== 'undefined') {
            // Wipe HUD boxes immediately; the backend also clears its snapshot.
            window.dispatchEvent(new CustomEvent('bawal-clear-detections'));
        }

        if (mode === 'mock') {
            // 1. Immediately request backend mode change and wipe frames/detections
            await fetch('/api/v1/camera/mode', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    mode: 'mock',
                    video_source: mockFileName,
                    video_uuid: mockVideoUUID
                })
            }).catch(() => {});

            // 2. Strict Pipeline Drain Delay: Wait 800ms so all YOLO detection boxes turn off
            // and the previous live camera stream is cleanly drained before mock frames start
            await new Promise(r => setTimeout(r, 800));

            if (mockVideoEl) {
                mockVideoEl.play().catch(() => {});
            }
            startMockPushing();
            isSwitchingMode = false;
        } else {
            // 1. Stop mock pushing and pause video immediately
            stopMockPushing();
            if (mockVideoEl) {
                mockVideoEl.pause();
            }

            // 2. Immediately request backend to switch to live mode and clear mock detections
            await fetch('/api/v1/camera/mode', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ mode: 'live' })
            }).catch(() => {});

            // 3. Strict Pipeline Drain Delay: Wait 800ms so mock detection boxes turn off
            // and live camera stream connects cleanly without leftover boxes
            await new Promise(r => setTimeout(r, 800));

            streamEpoch = Date.now();
            isSwitchingMode = false;
        }
    }

    function resetRewindState() {
        isRewound = false;
        lastVideoTime = 0;
        fetch('/api/v1/devices/mock/reset', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'X-Mock-Video-Source': mockFileName,
                'X-Mock-Video-UUID': mockVideoUUID,
                'X-Reinspect': '1',
                'X-Manual-Inspection': '1'
            },
            body: JSON.stringify({
                video_source: mockFileName,
                video_uuid: mockVideoUUID
            })
        }).catch(() => {});
        pushCurrentFrameOnce();
    }

    async function selectExample(name: string) {
        selectedVideo = name;
        mockFileName = name;
        isCustomUpload = false;
        isRewound = false;
        lastVideoTime = 0;
        isSwitchingMode = true;
        if (typeof localStorage !== 'undefined') {
            try { localStorage.setItem('bawal_mock_selected_video', name); } catch {}
        }
        const nextUUID = videoUUIDMap[name] || computeMockVideoUUID(name);
        await fetch('/api/v1/camera/mode', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                mode: 'mock',
                video_source: name,
                video_uuid: nextUUID
            })
        }).catch(() => {});
        if (mockVideoSrc.startsWith('blob:')) {
            URL.revokeObjectURL(mockVideoSrc);
        }
        mockVideoSrc = `/examples/${name}`;
        streamError = false;
        streamEpoch = Date.now();

        // 600ms pipeline drain delay when switching sample videos
        await new Promise(r => setTimeout(r, 600));
        if (mockVideoEl) {
            mockVideoEl.currentTime = 0;
            mockVideoEl.play().catch(() => {});
            pushCurrentFrameOnce();
        }
        isSwitchingMode = false;
    }

    function handleFileUpload(event: Event) {
        const input = event.target as HTMLInputElement;
        if (input.files && input.files[0]) {
            const file = input.files[0];
            mockFileName = file.name;
            customFileKey = `custom:${file.name}:${file.size}`;
            selectedVideo = 'custom';
            isCustomUpload = true;
            isRewound = false;
            lastVideoTime = 0;
            const nextUUID = computeMockVideoUUID(customFileKey);
            fetch('/api/v1/camera/mode', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    mode: 'mock',
                    video_source: file.name,
                    video_uuid: nextUUID
                })
            }).catch(() => {});
            if (mockVideoSrc.startsWith('blob:')) {
                URL.revokeObjectURL(mockVideoSrc);
            }
            mockVideoSrc = URL.createObjectURL(file);
            streamError = false;
            streamEpoch = Date.now();
            setTimeout(() => {
                if (mockVideoEl) {
                    mockVideoEl.currentTime = 0;
                    mockVideoEl.play().catch(() => {});
                    pushCurrentFrameOnce();
                }
            }, 80);
        }
    }

    function handleVideoLoaded() {
        if (mockVideoEl) {
            mockDuration = mockVideoEl.duration || 0;
            mockCurrentTime = mockVideoEl.currentTime || 0;
            lastVideoTime = mockCurrentTime;
            if (sourceMode === 'mock') {
                mockVideoEl.play().catch(() => {});
                startMockPushing();
                setTimeout(pushCurrentFrameOnce, 50);
            }
        }
    }

    function handleVideoTimeUpdate() {
        if (mockVideoEl) {
            const cur = mockVideoEl.currentTime;
            // Video loop or backward rewind detection
            if (cur < lastVideoTime - 0.3 || (lastVideoTime > 1.0 && cur < 0.5)) {
                isRewound = true;
            }
            lastVideoTime = cur;
            mockCurrentTime = cur;
        }
    }

    function togglePlayPause() {
        if (!mockVideoEl) return;
        if (mockVideoEl.paused) {
            mockVideoEl.play().catch(() => {});
            startMockPushing();
        } else {
            mockVideoEl.pause();
            stopMockPushing();
            pushCurrentFrameOnce();
        }
    }

    function handleSeek(e: Event) {
        const val = parseFloat((e.target as HTMLInputElement).value);
        if (mockVideoEl && !isNaN(val)) {
            if (val < mockCurrentTime - 0.2) {
                isRewound = true;
            }
            mockVideoEl.currentTime = val;
            mockCurrentTime = val;
            lastVideoTime = val;
            pushCurrentFrameOnce();
        }
    }

    function setPlaybackRate(rate: number) {
        mockPlaybackRate = rate;
        if (mockVideoEl) {
            mockVideoEl.playbackRate = rate;
        }
    }

    function formatTime(seconds: number): string {
        if (isNaN(seconds) || seconds < 0) return '00:00';
        const mins = Math.floor(seconds / 60);
        const secs = Math.floor(seconds % 60);
        return `${String(mins).padStart(2, '0')}:${String(secs).padStart(2, '0')}`;
    }

    onDestroy(() => {
        if (typeof window !== 'undefined') {
            window.removeEventListener('bawal-source-mode', handleGlobalSourceMode);
        }
        if (typeof document !== 'undefined') {
            document.removeEventListener('visibilitychange', handleVisibilityChange);
        }
        if (liveRefreshTimer !== null) {
            clearTimeout(liveRefreshTimer);
            liveRefreshTimer = null;
        }
        if (deviceStatusTimer !== null) {
            clearInterval(deviceStatusTimer);
            deviceStatusTimer = null;
        }
        stopMockPushing();
        if (mockVideoSrc.startsWith('blob:')) {
            URL.revokeObjectURL(mockVideoSrc);
        }
    });

    $effect(() => {
        if (hasCamera) {
            streamError = false;
        }
    });

    function getCategoryIcon(name: string): string {
        const lower = (name || '').toLowerCase();
        if (lower.includes('rice') || lower.includes('nasi')) return '🍚';
        if (lower.includes('chicken') || lower.includes('ayam')) return '🍗';
        if (lower.includes('egg') || lower.includes('telur')) return '🥚';
        if (lower.includes('tempeh') || lower.includes('tempe')) return '🧇';
        if (lower.includes('tofu') || lower.includes('tahu')) return '🧊';
        if (lower.includes('banana') || lower.includes('pisang')) return '🍌';
        if (lower.includes('orange') || lower.includes('jeruk')) return '🍊';
        if (lower.includes('water_spinach') || lower.includes('kangkung')) return '🌱';
        if (lower.includes('spinach') || lower.includes('bayam')) return '🥬';
        return '🍱';
    }

    let mealGroups = $derived.by(() => {
        const names = detections.map(d => (d.food_name || '').toLowerCase());
        const hasCarb = names.some(n => n.includes('rice') || n.includes('nasi'));
        const hasProtein = names.some(n => n.includes('chicken') || n.includes('ayam') || n.includes('egg') || n.includes('telur') || n.includes('tofu') || n.includes('tahu') || n.includes('tempeh') || n.includes('tempe'));
        const hasVeggie = names.some(n => n.includes('spinach') || n.includes('kangkung') || n.includes('bayam'));
        const hasFruit = names.some(n => n.includes('banana') || n.includes('pisang') || n.includes('orange') || n.includes('jeruk'));
        return {
            carb: hasCarb,
            protein: hasProtein,
            veggie: hasVeggie,
            fruit: hasFruit,
            count: [hasCarb, hasProtein, hasVeggie, hasFruit].filter(Boolean).length
        };
    });

    // Auto-recovery watchdog: auto-reconnect stream if stalled (Lambda safe: bounded backoff, max 3 retries)
    $effect(() => {
        if (!hasCamera || sourceMode !== 'live' || !streamError) return;
        if (retryCount >= MAX_RETRIES) return; // Halt auto-retries to prevent Lambda execution loop exhaustion

        const backoffMs = Math.min(3000 * Math.pow(1.5, retryCount), 10000);
        const timer = setTimeout(() => {
            retryCount += 1;
            streamEpoch = Date.now();
            streamError = false;
        }, backoffMs);

        return () => clearTimeout(timer);
    });

    // Detection Filter Tabs State & Boundary-Safe Status Helper
    type DetectionTabFilter = 'ALL' | 'PASS' | 'REVIEW' | 'REJECT';
    let detectionFilter = $state<DetectionTabFilter>('ALL');

    function getItemStatus(det: FoodDetection | null | undefined): 'PASS' | 'REVIEW' | 'REJECT' {
        if (!det) return 'PASS';
        
        // Priority 1: Explicit server or ML model status tag
        if (det.status === 'PASS' || det.status === 'REVIEW' || det.status === 'REJECT') {
            return det.status;
        }

        // Boundary-safe score parsing with clamped values [0..100]
        const rawSpoilage = Number(det.spoilage_score);
        const spoilage = Number.isFinite(rawSpoilage) ? Math.max(0, Math.min(100, rawSpoilage)) : 0;

        // Boundary-safe similarity rate parsing: handles 0..1 scale as well as 0..100% scale
        let rawSim = Number(det.similarity_rate);
        let sim = Number.isFinite(rawSim) ? rawSim : 1.0;
        if (sim > 1.0) sim = sim / 100.0;
        sim = Math.max(0, Math.min(1.0, sim));

        // Quality inspection decision boundaries
        if (spoilage >= 50 || sim < 0.35) return 'REJECT';
        if (spoilage >= 25 || sim < 0.60) return 'REVIEW';
        return 'PASS';
    }

    let validDetections = $derived(Array.isArray(detections) ? detections.filter((d): d is FoodDetection => Boolean(d)) : []);
    let passCount = $derived(validDetections.filter(d => getItemStatus(d) === 'PASS').length);
    let reviewCount = $derived(validDetections.filter(d => getItemStatus(d) === 'REVIEW').length);
    let rejectCount = $derived(validDetections.filter(d => getItemStatus(d) === 'REJECT').length);

    let filteredDetections = $derived.by(() => {
        if (detectionFilter === 'ALL') return validDetections;
        return validDetections.filter(d => getItemStatus(d) === detectionFilter);
    });
</script>

<div class="camera-stream-wrapper">
    <!-- Camera Viewport Card -->
    <div class="viewport-card">
        <div class="viewport-header">
            <div class="header-left">
                <span class="live-dot" class:offline={sourceMode === 'live' && (!hasCamera || streamError)}></span>
                <strong>{sourceMode === 'live' ? 'Live Inspection View' : 'Mock Video Inspection View (AI Connected)'}</strong>
            </div>

            <!-- Video / Camera Ingestion Source Mode Switcher -->
            <div class="source-mode-switcher">
                <button
                    type="button"
                    class="mode-pill-btn"
                    class:active={sourceMode === 'live'}
                    onclick={() => setSourceMode('live')}
                    title="Live continuous stream push from camera"
                >
                    <span class="pill-icon">📹</span> Live Stream
                </button>
                <button
                    type="button"
                    class="mode-pill-btn"
                    class:active={sourceMode === 'mock'}
                    onclick={() => setSourceMode('mock')}
                    title="Mock local MP4 test video with real ONNX AI pipeline"
                >
                    <span class="pill-icon">🎬</span> Mock Video
                </button>
            </div>

            <span class="pill-badge" class:badge-active={sourceMode === 'mock' || (hasCamera && !streamError)}>
                {#if sourceMode === 'live'}
                    {hasCamera && !streamError ? 'Live Push (Active HUD)' : 'Standby'}
                {:else}
                    🎬 MP4 -> ONNX AI Pipeline ({mockPlaybackRate}x)
                {/if}
            </span>
        </div>

        <div class="canvas-container">
            <!-- Hidden Canvas for Mock Video AI Frame Ingestion -->
            <canvas
                bind:this={mockCanvasEl}
                width="640"
                height="480"
                style="display: none;"
            ></canvas>

            {#if sourceMode === 'mock'}
                <!-- Smooth Native 60 FPS Mock Video Player -->
                <video
                    bind:this={mockVideoEl}
                    src={mockVideoSrc}
                    aria-label="Mock Video Feed"
                    class="feed-video"
                    crossorigin="anonymous"
                    playsinline
                    muted
                    loop
                    onloadedmetadata={handleVideoLoaded}
                    ontimeupdate={handleVideoTimeUpdate}
                    onplay={() => (mockIsPlaying = true)}
                    onpause={() => (mockIsPlaying = false)}
                ></video>

                <!-- Dynamic AI Bounding Box HUD Overlay for Mock Video (Turned OFF during mode switching transitions) -->
                {#if !isSwitchingMode && detections && detections.length > 0}
                    <svg class="hud-overlay" viewBox="0 0 640 480" preserveAspectRatio="xMidYMid meet">
                        {#each detections as det}
                            {#if det.box && det.box.length === 4}
                                {@const [bx, by, bw, bh] = det.box}
                                {@const status = getItemStatus(det)}
                                {@const strokeColor = status === 'PASS' ? '#10b981' : (status === 'REVIEW' ? '#f59e0b' : '#ef4444')}
                                <rect
                                    x={bx}
                                    y={by}
                                    width={bw}
                                    height={bh}
                                    fill="none"
                                    stroke={strokeColor}
                                    stroke-width="2.5"
                                    rx="4"
                                />
                                <rect
                                    x={bx}
                                    y={Math.max(0, by - 20)}
                                    width={Math.max(60, (det.food_name || '').length * 8 + 36)}
                                    height="20"
                                    fill={strokeColor}
                                    rx="3"
                                />
                                <text
                                    x={bx + 4}
                                    y={Math.max(14, by - 5)}
                                    fill="#ffffff"
                                    font-size="11"
                                    font-weight="bold"
                                    font-family="sans-serif"
                                >
                                    {det.food_name} {Math.round((det.similarity_rate || 0) * (det.similarity_rate <= 1 ? 100 : 1))}%
                                </text>
                            {/if}
                        {/each}
                    </svg>
                {/if}
            {:else}
                <!-- Live Stream Mode (Unified MJPEG Ingress from ESP32-S3 Camera) -->
                {#if hasCamera || !streamError}
                    <img
                        src={activeStreamUrl}
                        alt="AI Live Inspection Stream"
                        class="feed-video"
                        onload={() => { streamError = false; retryCount = 0; scheduleLiveRefresh(); }}
                        onerror={() => { streamError = true; }}
                    />
                {:else}
                    <!-- Standby State -->
                    <div class="standby-box">
                        <span class="standby-icon">📷</span>
                        <h4>No Camera Stream Connected</h4>
                        <p>Awaiting ESP32-S3 Camera Feed on /api/v1/camera/stream</p>
                        {#if streamError}
                            <button type="button" class="retry-btn" onclick={() => { streamError = false; retryCount = 0; streamEpoch = Date.now(); }}>
                                ↻ Reconnect Stream
                            </button>
                        {/if}
                    </div>
                {/if}
            {/if}

            {#if isSwitchingMode}
                <div class="mode-switching-scrim">
                    <div class="switching-spinner"></div>
                    <div class="switching-status-text">
                        <strong>Switching to {sourceMode === 'mock' ? 'Mock Simulation' : 'Live ESP32-S3'}</strong>
                        <span>Turning off YOLO detection boxes & synchronizing stream pipeline...</span>
                    </div>
                </div>
            {/if}
        </div>

        {#if sourceMode === 'mock'}
            <div class="mock-playback-panel">
                <!-- Play / Pause / Progress Scrubber Bar -->
                <div class="mock-player-row">
                    <button
                        type="button"
                        class="play-toggle-btn"
                        onclick={togglePlayPause}
                        title={mockIsPlaying ? 'Pause AI Video Processing' : 'Play Video Frame Ingestion'}
                    >
                        {#if mockIsPlaying}
                            <span>⏸ Pause</span>
                        {:else}
                            <span>▶ Play</span>
                        {/if}
                    </button>

                    <div class="scrubber-wrapper">
                        <input
                            type="range"
                            min="0"
                            max={mockDuration || 100}
                            step="0.05"
                            value={mockCurrentTime}
                            oninput={handleSeek}
                            class="mock-scrubber"
                            title="Seek video timeline"
                        />
                    </div>

                    <div class="time-readout">
                        <span>{formatTime(mockCurrentTime)}</span>
                        <span class="time-divider">/</span>
                        <span>{formatTime(mockDuration)}</span>
                    </div>

                    <!-- Speed Selector -->
                    <div class="speed-switcher">
                        {#each [0.5, 1.0, 1.5, 2.0] as rate}
                            <button
                                type="button"
                                class="speed-btn"
                                class:active={mockPlaybackRate === rate}
                                onclick={() => setPlaybackRate(rate)}
                            >
                                {rate}x
                            </button>
                        {/each}
                    </div>
                </div>

                <!-- Source Selection Bar -->
                <div class="mock-control-bar">
                    <div class="mock-left-cluster">
                        <div class="mock-select-group">
                            <label for="example-select-input" class="mock-label">
                                📁 <strong>examples/</strong>
                            </label>
                            <select
                                id="example-select-input"
                                class="example-select"
                                value={selectedVideo}
                                onchange={(e) => {
                                    const val = (e.target as HTMLSelectElement).value;
                                    if (val === 'custom') {
                                        fileInputRef?.click();
                                    } else {
                                        selectExample(val);
                                    }
                                }}
                            >
                                {#each exampleVideos as ex}
                                    <option value={ex}>{formatExampleLabel(ex)}</option>
                                {/each}
                                {#if isCustomUpload}
                                    <option value="custom">📎 {mockFileName} (Custom)</option>
                                {/if}
                            </select>
                        </div>

                        <div class="mock-actions">
                            <input
                                type="file"
                                accept="video/mp4,video/webm"
                                class="hidden-file-input"
                                bind:this={fileInputRef}
                                onchange={handleFileUpload}
                            />
                            <button
                                type="button"
                                class="upload-btn"
                                onclick={() => fileInputRef?.click()}
                                title="Upload custom MP4/WebM file from disk"
                            >
                                + Upload Custom Video
                            </button>
                        </div>
                    </div>

                    <div class="ai-status-indicator">
                        <span class="ai-dot-pulse" class:rewind={isRewound}></span>
                        <span class="ai-pipeline-text">
                            AI Pipeline: {mockIsPlaying ? 'Active Ingest' : 'Paused'} ({mockPushedFrames} frames)
                        </span>
                        {#if isRewound}
                            <span class="rewind-pill" title="Playback rewound or looped: Live bounding boxes active without duplicate database records or telemetry waste">🔁 Rewound (Data Preserved)</span>
                            <button type="button" class="reinspect-btn" onclick={resetRewindState} title="Trigger a fresh single-pass inspection for this sample">↺ Re-inspect</button>
                        {/if}
                    </div>
                </div>
            </div>
        {/if}

        <div class="viewport-footer">
            <div class="footer-left">
                <span class="device-label">📷 Device: {sourceMode === 'mock' ? `MOCK_VIDEO_CAM (UUID: ${mockVideoUUID.slice(0, 8)})` : (deviceId || 'ESP32S3_CAM')}</span>
                {#if sceneOptical && sceneOptical.lux_luminance != null}
                    <span class="optical-badge" title="SIMD AVX2 Optical Scene Analysis: Lux, White Balance & Correlated Color Temp">
                        💡 {Math.round(sceneOptical.lux_luminance)} lx • WB: {sceneOptical.white_balance_status} ({sceneOptical.color_temperature}K)
                    </span>
                {/if}
            </div>
            <div class="footer-right">
                <span class="ingest-label">
                    {#if sourceMode === 'live'}
                        ⚡ Ingestion: MJPEG Push (/api/v1/camera/stream)
                    {:else}
                        ⚡ Ingestion: MP4 Frame Ingestion &rarr; ONNX YOLOv8 &amp; Real-ESRGAN <span class="ingest-file">({mockFileName})</span>
                    {/if}
                </span>
            </div>
        </div>
    </div>

    <!-- Detected Items Live Panel -->
    <div class="entities-panel">
        <div class="panel-header">
            <div class="panel-title-group">
                <span class="entity-group-icon">🍱</span>
                <h3>Detected Items</h3>
            </div>
            <span class="active-count" class:has-items={validDetections.length > 0}>
                {validDetections.length} {validDetections.length === 1 ? 'Item' : 'Items'} Active
            </span>
        </div>

        <!-- Filter Tabs: All, Pass Detection, Review, Reject -->
        <div class="detection-tabs-row" role="tablist" aria-label="Detection Filter Tabs">
            <button
                type="button"
                role="tab"
                class="detection-tab-btn"
                class:active={detectionFilter === 'ALL'}
                onclick={() => (detectionFilter = 'ALL')}
            >
                <span>All</span>
                <span class="tab-badge">{validDetections.length}</span>
            </button>
            <button
                type="button"
                role="tab"
                class="detection-tab-btn tab-pass"
                class:active={detectionFilter === 'PASS'}
                onclick={() => (detectionFilter = 'PASS')}
                title="Filter Pass (Compliant) detections"
            >
                <span class="tab-dot dot-pass"></span>
                <span>Pass</span>
                <span class="tab-badge badge-pass">{passCount}</span>
            </button>
            <button
                type="button"
                role="tab"
                class="detection-tab-btn tab-review"
                class:active={detectionFilter === 'REVIEW'}
                onclick={() => (detectionFilter = 'REVIEW')}
                title="Filter Review warnings"
            >
                <span class="tab-dot dot-review"></span>
                <span>Review</span>
                <span class="tab-badge badge-review">{reviewCount}</span>
            </button>
            <button
                type="button"
                role="tab"
                class="detection-tab-btn tab-reject"
                class:active={detectionFilter === 'REJECT'}
                onclick={() => (detectionFilter = 'REJECT')}
                title="Filter Rejected detections"
            >
                <span class="tab-dot dot-reject"></span>
                <span>Reject</span>
                <span class="tab-badge badge-reject">{rejectCount}</span>
            </button>
        </div>

        <div class="entity-chips-grid">
            {#if validDetections.length === 0}
                <div class="empty-tray-state">
                    <span class="empty-icon">🍽️</span>
                    <strong>Awaiting Food Tray</strong>
                    <p>Place meal tray in camera view for real-time AI food detection & quality inspection.</p>
                </div>
            {:else if filteredDetections.length === 0}
                <div class="empty-tray-state">
                    <span class="empty-icon">🔍</span>
                    <strong>No {detectionFilter === 'PASS' ? 'Passing' : (detectionFilter === 'REVIEW' ? 'Review' : 'Rejected')} Items</strong>
                    <p>No active detections match the "{detectionFilter}" filter.</p>
                    <button type="button" class="reset-filter-btn" onclick={() => (detectionFilter = 'ALL')}>Show All Items</button>
                </div>
            {:else}
                {#each filteredDetections as det, idx}
                    {@const spoilage = Number(det.spoilage_score) || 0}
                    {@const freshness = Math.max(0, Math.min(100, Math.round(100 - spoilage)))}
                    {@const status = getItemStatus(det)}
                    
                    <div class="entity-item on" class:pass={status === 'PASS'} class:review={status === 'REVIEW'} class:reject={status === 'REJECT'}>
                        <div class="entity-icon-circle">
                            <span>{getCategoryIcon(det.food_name)}</span>
                        </div>
                        <div class="entity-info">
                            <div class="entity-top-row">
                                <span class="entity-name">{det.food_name}</span>
                                <span class="status-pill" class:pill-pass={status === 'PASS'} class:pill-review={status === 'REVIEW'} class:pill-reject={status === 'REJECT'}>
                                    {status}
                                </span>
                            </div>
                            <div class="entity-state-row">
                                <span class="entity-meta">
                                    {(det.similarity_rate * 100).toFixed(0)}% Conf
                                </span>
                                <span class="freshness-tag" class:tag-pass={status === 'PASS'} class:tag-review={status === 'REVIEW'} class:tag-reject={status === 'REJECT'}>
                                    {freshness}% Fresh
                                </span>
                            </div>
                        </div>
                    </div>
                {/each}
            {/if}
        </div>

        <!-- MBG Nutrition Balance Footer -->
        <div class="nutrition-footer">
            <div class="nutrition-header">
                <span class="nutrition-title">MBG Tray Balance</span>
                <span class="nutrition-badge" class:complete={mealGroups.count === 4}>
                    {mealGroups.count}/4 Groups
                </span>
            </div>
            <div class="nutrition-chips">
                <span class="nutri-chip" class:active={mealGroups.carb}>🍚 Carbs</span>
                <span class="nutri-chip" class:active={mealGroups.protein}>🍗 Protein</span>
                <span class="nutri-chip" class:active={mealGroups.veggie}>🥬 Veggies</span>
                <span class="nutri-chip" class:active={mealGroups.fruit}>🍊 Fruit</span>
            </div>
        </div>
    </div>
</div>

<style>
    .camera-stream-wrapper {
        display: grid;
        grid-template-columns: minmax(0, 1fr) minmax(260px, 300px);
        gap: 16px;
    }

    .viewport-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        overflow: hidden;
        display: flex;
        flex-direction: column;
        box-shadow: var(--shadow-card);
    }

    .viewport-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 12px 16px;
        background: var(--bg-card);
        border-bottom: 1px solid var(--border-subtle);
    }

    .header-left {
        display: flex;
        align-items: center;
        gap: 8px;
        font-size: 0.88rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .live-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: var(--accent-green);
        box-shadow: 0 0 8px var(--accent-green);
    }

    .live-dot.offline {
        background: var(--text-muted);
        box-shadow: none;
    }

    .source-mode-switcher {
        display: flex;
        background: #f1f5f9;
        padding: 2px;
        border-radius: var(--radius-sm);
        border: 1px solid var(--border-subtle);
        gap: 2px;
    }

    .mode-pill-btn {
        display: inline-flex;
        align-items: center;
        gap: 4px;
        font-size: 0.68rem;
        font-weight: 600;
        padding: 4px 10px;
        border-radius: var(--radius-sm);
        background: transparent;
        color: var(--text-secondary);
        border: none;
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .mode-pill-btn:hover {
        color: var(--text-primary);
        background: #e2e8f0;
    }

    .mode-pill-btn.active {
        background: #3679a8;
        color: #ffffff;
        font-weight: 700;
        box-shadow: 0 1px 2px rgba(54, 121, 168, 0.25);
    }

    .pill-icon {
        font-size: 0.75rem;
    }

    .pill-badge.badge-active {
        background: rgba(16, 185, 129, 0.15);
        color: #059669;
        font-weight: 700;
        border: 1px solid rgba(16, 185, 129, 0.3);
    }

    .mock-playback-panel {
        display: flex;
        flex-direction: column;
        background: #f8fafc;
        border-top: 1px solid var(--border-subtle);
    }

    .mock-player-row {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 8px 16px;
        border-bottom: 1px solid #e2e8f0;
    }

    .play-toggle-btn {
        background: #3679a8;
        color: #ffffff;
        border: none;
        border-radius: var(--radius-sm);
        padding: 5px 12px;
        font-size: 0.75rem;
        font-weight: 700;
        cursor: pointer;
        display: inline-flex;
        align-items: center;
        gap: 4px;
        transition: background 0.15s ease;
        flex-shrink: 0;
    }

    .play-toggle-btn:hover {
        background: #2b6187;
    }

    .scrubber-wrapper {
        flex: 1;
        display: flex;
        align-items: center;
    }

    .mock-scrubber {
        width: 100%;
        accent-color: #3679a8;
        cursor: pointer;
        height: 6px;
    }

    .time-readout {
        font-family: monospace;
        font-size: 0.72rem;
        font-weight: 600;
        color: var(--text-secondary);
        white-space: nowrap;
        display: flex;
        align-items: center;
        gap: 3px;
        flex-shrink: 0;
    }

    .time-divider {
        color: #94a3b8;
    }

    .speed-switcher {
        display: flex;
        gap: 2px;
        background: #e2e8f0;
        padding: 2px;
        border-radius: var(--radius-sm);
        flex-shrink: 0;
    }

    .speed-btn {
        background: transparent;
        border: none;
        border-radius: 2px;
        font-size: 0.65rem;
        font-weight: 600;
        color: #475569;
        padding: 2px 6px;
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .speed-btn:hover {
        background: #cbd5e1;
        color: #0f172a;
    }

    .speed-btn.active {
        background: #3679a8;
        color: #ffffff;
        font-weight: 700;
    }

    .ai-status-indicator {
        display: flex;
        align-items: center;
        gap: 8px;
        font-size: 0.72rem;
        font-weight: 600;
        color: #059669;
        margin-left: auto;
        flex-wrap: wrap;
        flex-shrink: 0;
    }

    .ai-pipeline-text {
        white-space: nowrap;
    }

    .ai-dot-pulse {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        background: #10b981;
        box-shadow: 0 0 6px #10b981;
        animation: pulse-dot 1.5s infinite;
        flex-shrink: 0;
    }

    .ai-dot-pulse.rewind {
        background: #3b82f6;
        box-shadow: 0 0 6px #3b82f6;
    }

    .rewind-pill {
        display: inline-flex;
        align-items: center;
        padding: 2px 8px;
        font-size: 0.68rem;
        font-weight: 700;
        background: rgba(59, 130, 246, 0.12);
        color: #2563eb;
        border: 1px solid rgba(59, 130, 246, 0.3);
        border-radius: 4px;
        line-height: 1.2;
        white-space: nowrap;
    }

    .reinspect-btn {
        display: inline-flex;
        align-items: center;
        padding: 2px 8px;
        font-size: 0.68rem;
        font-weight: 600;
        background: #ffffff;
        color: #475569;
        border: 1px solid #cbd5e1;
        border-radius: 4px;
        cursor: pointer;
        line-height: 1.2;
        white-space: nowrap;
        transition: all 0.15s ease;
    }

    .reinspect-btn:hover {
        background: #f1f5f9;
        color: #0f172a;
        border-color: #94a3b8;
    }

    @keyframes pulse-dot {
        0% { transform: scale(0.95); opacity: 0.8; }
        50% { transform: scale(1.2); opacity: 1; }
        100% { transform: scale(0.95); opacity: 0.8; }
    }

    .mock-control-bar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        flex-wrap: wrap;
        padding: 8px 16px;
        background: #f8fafc;
        border-top: 1px solid var(--border-subtle);
        font-size: 0.75rem;
        gap: 8px 16px;
        min-height: 44px;
    }

    .mock-left-cluster {
        display: flex;
        align-items: center;
        gap: 12px;
        flex-wrap: wrap;
        flex: 1 1 auto;
        min-width: 0;
    }

    .mock-select-group {
        display: flex;
        align-items: center;
        gap: 8px;
        flex: 1 1 auto;
        min-width: 240px;
        max-width: 440px;
    }

    .mock-label {
        color: var(--text-secondary);
        white-space: nowrap;
        font-size: 0.72rem;
        font-weight: 600;
        flex-shrink: 0;
    }

    .example-select {
        background: #ffffff;
        color: var(--text-primary);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-sm);
        padding: 5px 12px;
        font-size: 0.75rem;
        font-weight: 600;
        outline: none;
        cursor: pointer;
        width: 100%;
        min-width: 180px;
        transition: border-color 0.15s ease;
    }

    .example-select:hover,
    .example-select:focus {
        border-color: #3679a8;
    }

    .mock-actions {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-shrink: 0;
    }

    .hidden-file-input {
        display: none;
    }

    .upload-btn {
        background: #ffffff;
        color: #3679a8;
        border: 1px solid #cbd5e1;
        border-radius: var(--radius-sm);
        padding: 5px 12px;
        font-size: 0.72rem;
        font-weight: 600;
        cursor: pointer;
        white-space: nowrap;
        transition: all 0.15s ease;
    }

    .upload-btn:hover {
        background: #3679a8;
        color: #ffffff;
    }

    .pill-badge {
        font-size: 0.7rem;
        font-weight: 600;
        background: #e2e8f0;
        color: var(--text-secondary);
        padding: 3px 10px;
        border-radius: var(--radius-sm);
    }

    .canvas-container {
        position: relative;
        width: 100%;
        background: #14171d;
        display: flex;
        align-items: center;
        justify-content: center;
        aspect-ratio: 4 / 3;
        overflow: hidden;
    }

    .feed-video {
        width: 100%;
        height: 100%;
        object-fit: contain;
        display: block;
        margin: auto;
    }

    .hud-overlay {
        position: absolute;
        top: 0;
        left: 0;
        width: 100%;
        height: 100%;
        pointer-events: none;
        z-index: 5;
    }

    .standby-box {
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        text-align: center;
        padding: 30px 20px;
        background: #181c24;
        border: 1px dashed var(--border-subtle);
        border-radius: var(--radius-lg);
        width: 80%;
        gap: 8px;
    }

    .standby-icon {
        font-size: 2.2rem;
    }

    .standby-box h4 {
        margin: 0;
        font-size: 0.95rem;
        color: var(--text-primary);
        font-weight: 700;
    }

    .standby-box p {
        margin: 0;
        font-size: 0.75rem;
        color: var(--text-muted);
    }

    .retry-btn {
        margin-top: 8px;
        background: var(--accent-blue);
        color: #0f172a;
        font-weight: 700;
        font-size: 0.75rem;
        border: none;
        padding: 6px 14px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        transition: opacity 0.15s ease;
    }

    .retry-btn:hover {
        opacity: 0.9;
    }

    .viewport-footer {
        display: flex;
        justify-content: space-between;
        align-items: center;
        flex-wrap: wrap;
        padding: 8px 16px;
        font-size: 0.72rem;
        color: var(--text-muted);
        background: var(--bg-card);
        border-top: 1px solid var(--border-subtle);
        gap: 8px 16px;
        min-height: 40px;
    }

    .footer-left {
        display: flex;
        align-items: center;
        gap: 10px;
        flex-wrap: wrap;
    }

    .device-label {
        font-weight: 600;
        color: var(--text-secondary);
        display: inline-flex;
        align-items: center;
    }

    .optical-badge {
        display: inline-flex;
        align-items: center;
        background: #f1f5f9;
        border: 1px solid #cbd5e1;
        border-radius: var(--radius-sm);
        padding: 2px 8px;
        font-size: 0.68rem;
        font-weight: 600;
        color: #334155;
        white-space: nowrap;
    }

    .footer-right {
        display: flex;
        align-items: center;
        gap: 6px;
        text-align: right;
        margin-left: auto;
    }

    .ingest-label {
        color: var(--text-secondary);
        font-size: 0.72rem;
        font-weight: 500;
    }

    .ingest-file {
        font-weight: 600;
        color: #3679a8;
    }

    .entities-panel {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        padding: 16px;
        display: flex;
        flex-direction: column;
        gap: 12px;
        box-shadow: var(--shadow-card);
        min-width: 0;
        overflow: hidden;
    }

    .panel-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 10px;
        border-bottom: 1px solid var(--border-subtle);
    }

    .panel-title-group {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .entity-group-icon {
        font-size: 1.1rem;
    }

    .panel-header h3 {
        margin: 0;
        font-size: 0.92rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .active-count {
        font-size: 0.7rem;
        font-weight: 600;
        background: var(--accent-blue-dim);
        color: var(--accent-blue);
        padding: 2px 8px;
        border-radius: var(--radius-pill);
    }

    .detection-tabs-row {
        display: grid;
        grid-template-columns: repeat(4, 1fr);
        background: #f1f5f9;
        padding: 3px;
        border-radius: var(--radius-sm);
        border: 1px solid var(--border-subtle);
        gap: 2px;
        width: 100%;
        box-sizing: border-box;
    }

    .detection-tab-btn {
        min-width: 0;
        display: inline-flex;
        align-items: center;
        justify-content: center;
        gap: 3px;
        font-size: 0.65rem;
        font-weight: 600;
        padding: 4px 2px;
        border-radius: var(--radius-sm);
        background: transparent;
        color: var(--text-secondary);
        border: none;
        cursor: pointer;
        transition: all 0.15s ease;
        white-space: nowrap;
        overflow: hidden;
    }

    .detection-tab-btn span {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .detection-tab-btn:hover {
        background: rgba(255, 255, 255, 0.7);
        color: var(--text-primary);
    }

    .detection-tab-btn.active {
        background: #ffffff;
        color: var(--text-primary);
        box-shadow: 0 1px 3px rgba(0, 0, 0, 0.08);
        font-weight: 700;
    }

    .detection-tab-btn.tab-pass.active {
        color: #059669;
    }

    .detection-tab-btn.tab-review.active {
        color: #d97706;
    }

    .detection-tab-btn.tab-reject.active {
        color: #dc2626;
    }

    .tab-dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
        display: inline-block;
        flex-shrink: 0;
    }

    .dot-pass {
        background: #10b981;
    }

    .dot-review {
        background: #f59e0b;
    }

    .dot-reject {
        background: #ef4444;
    }

    .tab-badge {
        font-size: 0.60rem;
        padding: 1px 3px;
        border-radius: 10px;
        background: rgba(148, 163, 184, 0.2);
        color: #475569;
        line-height: 1.1;
        flex-shrink: 0;
    }

    .badge-pass {
        background: rgba(16, 185, 129, 0.15);
        color: #059669;
    }

    .badge-review {
        background: rgba(245, 158, 11, 0.15);
        color: #d97706;
    }

    .badge-reject {
        background: rgba(239, 68, 68, 0.15);
        color: #dc2626;
    }

    .reset-filter-btn {
        margin-top: 8px;
        padding: 4px 10px;
        font-size: 0.68rem;
        font-weight: 600;
        background: #ffffff;
        color: #3679a8;
        border: 1px solid #cbd5e1;
        border-radius: var(--radius-sm);
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .reset-filter-btn:hover {
        background: #3679a8;
        color: #ffffff;
    }

    .entity-chips-grid {
        display: flex;
        flex-direction: column;
        gap: 6px;
        overflow-y: auto;
    }

    .entity-item {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 8px 10px;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        transition: all 0.15s ease;
    }

    .entity-item.on {
        background: rgba(56, 189, 248, 0.08);
        border-color: rgba(56, 189, 248, 0.4);
    }

    .entity-icon-circle {
        width: 32px;
        height: 32px;
        border-radius: 50%;
        background: var(--bg-chip);
        display: flex;
        align-items: center;
        justify-content: center;
        font-size: 0.95rem;
        flex-shrink: 0;
        transition: all 0.15s ease;
    }

    .entity-item.on .entity-icon-circle {
        background: var(--accent-blue);
    }

    .entity-info {
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }

    .entity-name {
        font-size: 0.78rem;
        font-weight: 600;
        color: var(--text-secondary);
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }

    .entity-item.on .entity-name {
        color: var(--text-primary);
        font-weight: 700;
    }

    .entity-state-row {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-top: 1px;
    }

    .freshness-tag {
        font-size: 0.62rem;
        font-weight: 700;
        padding: 1px 6px;
        border-radius: 4px;
        line-height: 1.2;
    }

    .freshness-tag.tag-pass {
        background: rgba(34, 197, 94, 0.15);
        color: #22c55e;
        border: 1px solid rgba(34, 197, 94, 0.3);
    }

    .freshness-tag.tag-review {
        background: rgba(245, 158, 11, 0.15);
        color: #f59e0b;
        border: 1px solid rgba(245, 158, 11, 0.3);
    }
    .entity-top-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 6px;
    }

    .status-pill {
        font-size: 0.60rem;
        font-weight: 800;
        letter-spacing: 0.03em;
        padding: 1px 6px;
        border-radius: 4px;
        text-transform: uppercase;
    }

    .status-pill.pill-pass {
        background: rgba(34, 197, 94, 0.18);
        color: #22c55e;
    }

    .status-pill.pill-review {
        background: rgba(245, 158, 11, 0.18);
        color: #f59e0b;
    }

    .status-pill.pill-reject {
        background: rgba(239, 68, 68, 0.18);
        color: #ef4444;
    }

    .entity-meta {
        font-size: 0.65rem;
        color: var(--text-muted);
        font-weight: 500;
    }

    .empty-tray-state {
        display: flex;
        flex-direction: column;
        align-items: center;
        text-align: center;
        padding: 24px 12px;
        background: var(--bg-inset);
        border: 1px dashed var(--border-subtle);
        border-radius: var(--radius-md);
        gap: 6px;
    }

    .empty-icon {
        font-size: 1.8rem;
    }

    .empty-tray-state strong {
        font-size: 0.82rem;
        color: var(--text-primary);
    }

    .empty-tray-state p {
        margin: 0;
        font-size: 0.68rem;
        color: var(--text-muted);
        line-height: 1.3;
    }

    .active-count.has-items {
        background: rgba(34, 197, 94, 0.15);
        color: #22c55e;
        border: 1px solid rgba(34, 197, 94, 0.3);
    }

    .nutrition-footer {
        margin-top: auto;
        padding-top: 10px;
        border-top: 1px solid var(--border-subtle);
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .nutrition-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .nutrition-title {
        font-size: 0.70rem;
        font-weight: 700;
        color: var(--text-muted);
        text-transform: uppercase;
        letter-spacing: 0.04em;
    }

    .nutrition-badge {
        font-size: 0.65rem;
        font-weight: 700;
        background: var(--bg-chip);
        color: var(--text-secondary);
        padding: 1px 6px;
        border-radius: var(--radius-pill);
    }

    .nutrition-badge.complete {
        background: rgba(34, 197, 94, 0.15);
        color: #22c55e;
    }

    .nutrition-chips {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 4px;
    }

    .nutri-chip {
        font-size: 0.65rem;
        padding: 4px 6px;
        border-radius: 4px;
        background: var(--bg-inset);
        color: var(--text-muted);
        border: 1px solid var(--border-subtle);
        display: flex;
        align-items: center;
        gap: 4px;
        opacity: 0.55;
    }

    .nutri-chip.active {
        background: rgba(56, 189, 248, 0.1);
        color: var(--text-primary);
        border-color: rgba(56, 189, 248, 0.4);
        font-weight: 600;
        opacity: 1.0;
    }

    @media (max-width: 900px) {
        .camera-stream-wrapper {
            grid-template-columns: 1fr;
        }
    }

    @media (max-width: 640px) {
        .viewport-header {
            flex-wrap: wrap;
            gap: 8px;
            padding: 10px 12px;
        }

        .source-mode-switcher {
            order: 3;
            width: 100%;
        }

        .mode-pill-btn {
            flex: 1;
            justify-content: center;
            padding: 6px 10px;
        }

        .mock-control-bar {
            flex-direction: column;
            align-items: stretch;
            gap: 8px;
            padding: 10px 12px;
        }

        .mock-select-group {
            width: 100%;
        }

        .example-select {
            flex: 1;
            max-width: none;
            width: 100%;
        }

        .mock-actions,
        .upload-btn {
            width: 100%;
            text-align: center;
        }

        .viewport-footer {
            flex-direction: column;
            gap: 4px;
            padding: 8px 12px;
        }
    }

    .mode-switching-scrim {
        position: absolute;
        inset: 0;
        background: rgba(15, 23, 42, 0.78);
        backdrop-filter: blur(4px);
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        gap: 12px;
        z-index: 40;
        animation: fadeInScrim 0.15s ease-out;
    }

    .switching-spinner {
        width: 36px;
        height: 36px;
        border: 3px solid rgba(255, 255, 255, 0.2);
        border-top-color: #38bdf8;
        border-radius: 50%;
        animation: spinScrim 0.7s linear infinite;
    }

    .switching-status-text {
        display: flex;
        flex-direction: column;
        align-items: center;
        text-align: center;
        color: #ffffff;
        gap: 4px;
    }

    .switching-status-text strong {
        font-size: 0.95rem;
        font-weight: 600;
        letter-spacing: -0.01em;
        color: #f8fafc;
    }

    .switching-status-text span {
        font-size: 0.78rem;
        color: #94a3b8;
    }

    @keyframes fadeInScrim {
        from { opacity: 0; }
        to { opacity: 1; }
    }

    @keyframes spinScrim {
        to { transform: rotate(360deg); }
    }
</style>
