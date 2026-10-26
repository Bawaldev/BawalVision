<script lang="ts">
    import { onMount, onDestroy, tick } from 'svelte';
    import { env as publicEnv } from '$env/dynamic/public';
    import { resolveApiBase, setConfiguredApiBase } from '../backendConfig';
    import type { FoodDetection, SceneOpticalMetrics } from '../types';

    interface Props {
        detections: FoodDetection[];
        deviceId?: string;
        isLive?: boolean;
        hasCamera?: boolean;
        isActive?: boolean;
        streamUrl?: string;
        sceneOptical?: SceneOpticalMetrics;
    }

    let {
        detections = [],
        deviceId = '',
        isLive = true,
        hasCamera = false,
        isActive = true,
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

    type SourceModeType = 'live' | 'mock' | 'device_cam';
    let sourceMode = $state<SourceModeType>('live');

    let activeApiBase = $state(resolveApiBase());

    function withApiOrigin(path: string, base = activeApiBase): string {
        if (!path || /^(https?:|blob:|data:)/i.test(path)) return path;
        const origin = base === '/api/v1' ? '' : base.replace(/\/api\/v1$/i, '');
        return origin ? `${origin}${path.startsWith('/') ? path : `/${path}`}` : path;
    }

    function apiEndpoint(path: string): string {
        const base = resolveApiBase();
        if (base === '/api/v1') return path;
        return `${base.replace(/\/api\/v1$/i, '')}${path}`;
    }

    function syncApiBase() {
        const next = resolveApiBase();
        if (next !== activeApiBase) {
            activeApiBase = next;
            streamEpoch = Date.now();
        }
    }

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
    let resolvedMockVideoSrc = $derived(
        mockVideoSrc.startsWith('blob:') || mockVideoSrc.startsWith('http')
            ? mockVideoSrc
            : (mockVideoSrc.startsWith('/') ? mockVideoSrc : `/${mockVideoSrc}`)
    );
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
    let mockAnnotatedFrameUrl = $state<string>('');
    let deviceCamAnnotatedUrl = $state<string>('');

    // Device / Phone Camera Ingestion Pipeline State
    let deviceCamVideoEl = $state<HTMLVideoElement | null>(null);
    let deviceCamStream: MediaStream | null = null;
    let isDeviceCamActive = $state(false);
    let deviceCamError = $state('');

    async function startDeviceCam() {
        stopDeviceCam();
        deviceCamError = '';
        if (typeof navigator === 'undefined' || !navigator.mediaDevices?.getUserMedia) {
            deviceCamError = 'Camera access (getUserMedia) is not supported in this browser environment.';
            return;
        }
        try {
            let stream: MediaStream | null = null;
            try {
                // Primary: Try rear/environment camera on smartphone
                stream = await navigator.mediaDevices.getUserMedia({
                    video: {
                        facingMode: { ideal: 'environment' },
                        width: { ideal: 640 },
                        height: { ideal: 480 }
                    },
                    audio: false
                });
            } catch {
                // Fallback: Default webcam for laptops / PCs without environment camera
                stream = await navigator.mediaDevices.getUserMedia({
                    video: {
                        width: { ideal: 640 },
                        height: { ideal: 480 }
                    },
                    audio: false
                });
            }

            if (!stream) {
                throw new Error('Unable to acquire camera video stream.');
            }

            deviceCamStream = stream;
            isDeviceCamActive = true;
            await tick();
            if (deviceCamVideoEl) {
                deviceCamVideoEl.srcObject = stream;
                try {
                    await deviceCamVideoEl.play();
                } catch {}
            }
            startLivePushing();
            if (typeof window !== 'undefined') {
                window.dispatchEvent(new CustomEvent('bawal-camera-online', {
                    detail: { active: true, deviceId: 'PHONE_CAMERA' }
                }));
            }
        } catch (err: any) {
            console.error('Failed to open device camera:', err);
            deviceCamError = err?.name === 'NotAllowedError'
                ? 'Camera access was denied. Please allow camera permissions in your browser.'
                : (err?.message || 'Failed to initialize device camera.');
            isDeviceCamActive = false;
        }
    }

    $effect(() => {
        if (sourceMode === 'device_cam' && isDeviceCamActive && deviceCamVideoEl && deviceCamStream) {
            if (deviceCamVideoEl.srcObject !== deviceCamStream) {
                deviceCamVideoEl.srcObject = deviceCamStream;
                deviceCamVideoEl.play().catch(() => {});
            }
        }
    });

    function stopDeviceCam() {
        if (deviceCamStream) {
            deviceCamStream.getTracks().forEach((track) => track.stop());
            deviceCamStream = null;
        }
        if (deviceCamVideoEl) {
            deviceCamVideoEl.srcObject = null;
        }
        isDeviceCamActive = false;
    }

    let streamError = $state(false);
    let streamEpoch = $state(Date.now());
    let discoveredStreamUrl = $state('');
    let manualEspStreamUrl = $state('');
    let autoDetectedEspIp = $state('');
    let autoDetectedBackendIp = $state('');
    let isAttemptingLocalStream = $state(true);
    let isHttpsDomain = $derived(typeof window !== 'undefined' && window.location.protocol === 'https:');
    let handleExternalCameraOnline: ((e: any) => void) | null = null;

    function escapeHtml(str: string): string {
        return str
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#039;');
    }

    function isValidStreamHost(url: string): boolean {
        if (!url) return false;
        const match = url.match(/https?:\/\/([0-9.]+)(?::\d+)?/);
        if (!match) return url.includes('localhost') || url.includes('.local');
        const ip = match[1];
        const parts = ip.split('.');
        if (parts.length !== 4) return false;
        return parts.every(p => {
            const n = parseInt(p, 10);
            return !isNaN(n) && n >= 0 && n <= 255;
        });
    }

    function connectManualEspStream() {
        if (!manualEspStreamUrl.trim()) return;
        const raw = manualEspStreamUrl.trim();
        const safeRaw = escapeHtml(raw);

        if (/^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/.test(raw)) {
            localBackendNotice = `⚠️ <strong><code>${safeRaw}</code> is a MAC Address, NOT an IP Address!</strong><br/>` +
                `In your phone's Hotspot list, tap on this device to see its <strong>IP Address</strong> (formatted like <code>192.168.43.xxx</code>).`;
            return;
        }

        let url = raw;
        if (!/^https?:\/\//i.test(url)) {
            url = `http://${url}`;
        }
        if (!url.includes('/stream') && !url.includes(':8080')) {
            url = url.replace(/\/+$/, '') + '/stream';
        }

        if (!isValidStreamHost(url)) {
            localBackendNotice = `⚠️ <strong>Invalid IP address format: <code>${safeRaw}</code></strong>.<br/>` +
                `An IPv4 address must have 4 octets between 0-255 (e.g. <code>192.168.1.150</code> as shown on your node LCD).`;
            return;
        }

        manualEspStreamUrl = url;
        if (typeof localStorage !== 'undefined') {
            try { localStorage.setItem('bawal_esp32_stream_url', url); } catch {}
        }
        discoveredStreamUrl = url;
        streamError = false;
        retryCount = 0;
        mockAnnotatedFrameUrl = '';
        deviceCamAnnotatedUrl = '';
        isLiveFeedActive = true;
        streamEpoch = Date.now();
        startLivePushing();
        localBackendNotice = `Connecting to stream at <code>${url}</code>...`;
        if (typeof window !== 'undefined') {
            window.dispatchEvent(new CustomEvent('bawal-camera-online', {
                detail: { active: true, deviceId: 'ESP32S3_441BF6C95678' }
            }));
        }

        // Concurrently probe the active C++ backend on port 8080 to sync real ESP-IDF hardware telemetry
        // Skip on HTTPS pages (e.g. Vercel) where browser Private Network Access blocks loopback requests
        if (typeof window !== 'undefined' && window.location.protocol === 'https:') {
            return;
        }

        const hostMatch = url.match(/https?:\/\/([^/:]+)/i);
        const host = hostMatch ? hostMatch[1] : '';
        const backendCandidates = [
            host ? `http://${host}:8080/api/v1` : '',
            'http://localhost:8080/api/v1',
            'http://127.0.0.1:8080/api/v1'
        ].filter(Boolean);

        (async () => {
            for (const cand of backendCandidates) {
                try {
                    const c = new AbortController();
                    const to = setTimeout(() => c.abort(), 1200);
                    const r = await fetch(`${cand}/telemetry/latest`, { signal: c.signal, cache: 'no-store' });
                    clearTimeout(to);
                    if (r.ok) {
                        const data = await r.json().catch(() => null);
                        if (data && (data.is_connected || data.temperature != null)) {
                            setConfiguredApiBase(cand);
                            activeApiBase = cand;
                            if (typeof window !== 'undefined') {
                                window.dispatchEvent(new CustomEvent('bawal-backend-updated', { detail: cand }));
                            }
                            localBackendNotice = `✓ Live camera stream connected! Real ESP-IDF hardware telemetry active from <code>${cand}</code> (${data.device_id || 'ESP32S3_441BF6C95678'}).`;
                            break;
                        }
                    }
                } catch {}
            }
        })();
    }
    let liveImgEl = $state<HTMLImageElement | null>(null);
    let isLiveFeedActive = $state(false);
    let isPushingLiveFrame = false;
    let livePushTimer: ReturnType<typeof setInterval> | null = null;
    let livePushKickoff: ReturnType<typeof setTimeout> | null = null;
    let livePushIntervalMs = 0;
    let liveFrameSeq = 0;
    let deviceStatusTimer: ReturnType<typeof setInterval> | null = null;
    let retryCount = $state(0);
    const MAX_RETRIES = 3;
    let isTabVisible = $state(true);

    let wsLiveStream: WebSocket | null = null;
    let wsLiveFrameUrl = $state('');
    let relayReconnectTimer: ReturnType<typeof setTimeout> | null = null;

    const relayBaseUrl = (publicEnv.PUBLIC_CAMERA_RELAY_URL || '').trim();
    const relayDeviceId = (publicEnv.PUBLIC_CAMERA_RELAY_DEVICE || 'default').trim() || 'default';
    const relayViewKey = (publicEnv.PUBLIC_CAMERA_RELAY_VIEW_KEY || '').trim();
    const relayConfigured = relayBaseUrl.length > 0;

    function relayWatchUrl(): string {
        let base = relayBaseUrl;
        if (/^https:\/\//i.test(base)) base = `wss://${base.slice(8)}`;
        else if (/^http:\/\//i.test(base)) base = `ws://${base.slice(7)}`;
        else if (!/^wss?:\/\//i.test(base)) base = `wss://${base}`;
        if (!/^wss?:\/\/[^/]+\/[^/]/.test(base)) {
            base = `${base.replace(/\/+$/, '')}/watch`;
        }
        const query = new URLSearchParams({ device: relayDeviceId });
        if (relayViewKey) query.set('key', relayViewKey);
        return `${base}${base.includes('?') ? '&' : '?'}${query.toString()}`;
    }

    function applyLiveFrame(event: MessageEvent, socket: WebSocket) {
        if (wsLiveStream !== socket) return;
        if (!(event.data instanceof ArrayBuffer) || event.data.byteLength <= 4) return;
        const blob = new Blob([event.data], { type: 'image/jpeg' });
        if (wsLiveFrameUrl) {
            URL.revokeObjectURL(wsLiveFrameUrl);
        }
        wsLiveFrameUrl = URL.createObjectURL(blob);
        isLiveFeedActive = true;
        streamError = false;
        localBackendNotice = '';
    }

    function scheduleRelayReconnect(attempt: number) {
        if (relayReconnectTimer !== null) return;
        const delay = Math.min(15000, 2000 * Math.max(1, attempt));
        relayReconnectTimer = setTimeout(() => {
            relayReconnectTimer = null;
            if (sourceMode === 'live' && isActive && isTabVisible) {
                connectRelayWebSocket(attempt);
            }
        }, delay);
    }

    function connectRelayWebSocket(attempt = 0) {
        if (!relayConfigured || typeof window === 'undefined') return;
        if (!isActive || !isTabVisible || sourceMode !== 'live') return;
        if (relayReconnectTimer !== null) {
            clearTimeout(relayReconnectTimer);
            relayReconnectTimer = null;
        }

        try {
            const socket = new WebSocket(relayWatchUrl());
            socket.binaryType = 'arraybuffer';
            wsLiveStream = socket;
            let opened = false;

            socket.onopen = () => {
                if (wsLiveStream !== socket) return;
                opened = true;
                localBackendNotice = '✓ Cloud camera relay connected. Waiting for ESP32 frames…';
            };
            socket.onmessage = (event) => applyLiveFrame(event, socket);
            socket.onerror = () => {};
            socket.onclose = () => {
                if (wsLiveStream === socket) wsLiveStream = null;
                if (!isActive || !isTabVisible || sourceMode !== 'live') return;
                if (!opened && attempt === 0) {
                    localBackendNotice = '⚠️ Cloud camera relay is unreachable. Check <code>PUBLIC_CAMERA_RELAY_URL</code> and the relay deployment.';
                    connectBackendLiveWebSocket();
                    return;
                }
                scheduleRelayReconnect(attempt + 1);
            };
        } catch {
            connectBackendLiveWebSocket();
        }
    }

    function connectBackendLiveWebSocket() {
        if (typeof window === 'undefined') return;
        disconnectLiveWebSocket();

        // Vercel serverless does not host stateful WebSockets; skip if no external backend URL is configured
        if (window.location.hostname.endsWith('.vercel.app') && !activeApiBase.startsWith('http')) {
            return;
        }

        const base = activeApiBase.replace(/\/+$/, '');
        const wsOrigin = base.startsWith('http')
            ? base.replace(/^http/i, 'ws')
            : (window.location.protocol === 'https:' ? 'wss://' : 'ws://') + window.location.host + base;
        const targetWsUrl = `${wsOrigin}/camera/ws/stream`;

        try {
            const socket = new WebSocket(targetWsUrl);
            wsLiveStream = socket;
            socket.binaryType = 'arraybuffer';
            socket.onmessage = (event) => applyLiveFrame(event, socket);
            socket.onerror = () => {
                // Graceful fallback to HTTP MJPEG stream
            };
            socket.onclose = () => {
                if (wsLiveStream === socket) wsLiveStream = null;
            };
        } catch {}
    }

    function connectLiveWebSocket() {
        if (typeof window === 'undefined') return;
        disconnectLiveWebSocket();
        if (relayConfigured) {
            connectRelayWebSocket();
            return;
        }
        connectBackendLiveWebSocket();
    }

    function disconnectLiveWebSocket() {
        if (relayReconnectTimer !== null) {
            clearTimeout(relayReconnectTimer);
            relayReconnectTimer = null;
        }
        if (wsLiveStream) {
            const socket = wsLiveStream;
            wsLiveStream = null;
            try { socket.close(); } catch {}
        }
        if (wsLiveFrameUrl) {
            URL.revokeObjectURL(wsLiveFrameUrl);
            wsLiveFrameUrl = '';
        }
    }

    let isBlobStreamRunning = false;
    let blobStreamAbortController: AbortController | null = null;

    async function runBlobStream(targetStreamUrl: string) {
        if (!targetStreamUrl) return;
        stopBlobStream();
        isBlobStreamRunning = true;

        const devToken = typeof localStorage !== 'undefined' ? localStorage.getItem('bawal_device_token') || '' : '';

        // Derive stream and snapshot endpoints
        let streamEndpoint = targetStreamUrl;
        if (!streamEndpoint.includes('/stream') && !streamEndpoint.includes(':8080')) {
            streamEndpoint = streamEndpoint.replace(/\/+$/, '') + '/stream';
        }
        if (devToken && !streamEndpoint.includes('token=')) {
            const sep = streamEndpoint.includes('?') ? '&' : '?';
            streamEndpoint = `${streamEndpoint}${sep}token=${encodeURIComponent(devToken)}`;
        }

        let snapshotEndpoint = targetStreamUrl.replace(/\/stream(?:\?.*)?$/i, '/snapshot');
        if (!snapshotEndpoint.includes('/snapshot')) {
            snapshotEndpoint = snapshotEndpoint.replace(/\/+$/, '') + '/snapshot';
        }
        if (devToken && !snapshotEndpoint.includes('token=')) {
            const sep = snapshotEndpoint.includes('?') ? '&' : '?';
            snapshotEndpoint = `${snapshotEndpoint}${sep}token=${encodeURIComponent(devToken)}`;
        }

        // Strategy 1: High-Performance Continuous Multipart Stream Reader (15-20 FPS)
        // Uses a single persistent TCP connection via ReadableStream, eliminating per-frame HTTP handshakes
        try {
            blobStreamAbortController = new AbortController();
            const res = await fetch(streamEndpoint, {
                signal: blobStreamAbortController.signal,
                cache: 'no-store',
                mode: 'cors'
            });

            if (res.ok && res.body && typeof res.body.getReader === 'function') {
                const reader = res.body.getReader();
                let chunkBuffer = new Uint8Array(0);

                while (isBlobStreamRunning && isTabVisible && isActive && sourceMode === 'live') {
                    const { done, value } = await reader.read();
                    if (done) break;
                    if (!value || value.length === 0) continue;

                    const merged = new Uint8Array(chunkBuffer.length + value.length);
                    merged.set(chunkBuffer, 0);
                    merged.set(value, chunkBuffer.length);
                    chunkBuffer = merged;

                    // Locate JPEG Start of Image (0xFF 0xD8) and End of Image (0xFF 0xD9)
                    let startIndex = -1;
                    for (let i = 0; i < chunkBuffer.length - 1; i++) {
                        if (chunkBuffer[i] === 0xFF && chunkBuffer[i + 1] === 0xD8) {
                            startIndex = i;
                            break;
                        }
                    }

                    if (startIndex !== -1) {
                        let endIndex = -1;
                        for (let i = startIndex + 2; i < chunkBuffer.length - 1; i++) {
                            if (chunkBuffer[i] === 0xFF && chunkBuffer[i + 1] === 0xD9) {
                                endIndex = i + 2;
                                break;
                            }
                        }

                        if (endIndex !== -1) {
                            const jpegBytes = chunkBuffer.slice(startIndex, endIndex);
                            chunkBuffer = chunkBuffer.slice(endIndex);

                            if (jpegBytes.length > 500) {
                                const blob = new Blob([jpegBytes], { type: 'image/jpeg' });
                                const newBlobUrl = URL.createObjectURL(blob);
                                if (wsLiveFrameUrl) {
                                    URL.revokeObjectURL(wsLiveFrameUrl);
                                }
                                wsLiveFrameUrl = newBlobUrl;
                                isLiveFeedActive = true;
                                streamError = false;
                                localBackendNotice = '';
                            }
                        }
                    }

                    // Prevent buffer overflow if stream is noisy
                    if (chunkBuffer.length > 500000) {
                        chunkBuffer = new Uint8Array(0);
                    }
                }
                return;
            }
        } catch {
            // Fallback to Strategy 2 (Snapshot Polling) if multipart stream is closed or unsupported
        }

        // Strategy 2: Fast Snapshot Polling Fallback
        while (isBlobStreamRunning && isTabVisible && isActive && sourceMode === 'live') {
            try {
                blobStreamAbortController = new AbortController();
                const timeoutId = setTimeout(() => blobStreamAbortController?.abort(), 2000);

                const res = await fetch(snapshotEndpoint, {
                    signal: blobStreamAbortController.signal,
                    cache: 'no-store',
                    mode: 'cors'
                });
                clearTimeout(timeoutId);

                if (res.ok) {
                    const blob = await res.blob();
                    if (blob && blob.size > 200) {
                        const newBlobUrl = URL.createObjectURL(blob);
                        if (wsLiveFrameUrl) {
                            URL.revokeObjectURL(wsLiveFrameUrl);
                        }
                        wsLiveFrameUrl = newBlobUrl;
                        isLiveFeedActive = true;
                        streamError = false;
                        localBackendNotice = '';
                    }
                    // Yield ~10ms to prevent main thread blocking while allowing high FPS
                    await new Promise((resolve) => setTimeout(resolve, 10));
                } else {
                    await new Promise((resolve) => setTimeout(resolve, 600));
                }
            } catch {
                if (!isBlobStreamRunning) break;
                await new Promise((resolve) => setTimeout(resolve, 500));
            }
        }
        isBlobStreamRunning = false;
    }

    function stopBlobStream() {
        isBlobStreamRunning = false;
        if (blobStreamAbortController) {
            blobStreamAbortController.abort();
            blobStreamAbortController = null;
        }
    }

    $effect(() => {
        const stream = discoveredStreamUrl || manualEspStreamUrl || streamUrl;
        if (!relayConfigured && isHttpsDomain && sourceMode === 'live' && isTabVisible && isActive && stream && stream.startsWith('http://')) {
            runBlobStream(stream);
        } else if (!isActive || !isTabVisible || sourceMode !== 'live' || relayConfigured) {
            stopBlobStream();
        }
    });

    let activeStreamUrl = $derived.by(() => {
        if (!isTabVisible || !isActive) return '';
        if (relayConfigured) return '';
        if (streamUrl) {
            let url = withApiOrigin(streamUrl, activeApiBase);
            if (url && url.includes('/stream') && !url.includes(':8080') && !url.includes('token=')) {
                const devToken = typeof localStorage !== 'undefined' ? localStorage.getItem('bawal_device_token') || '' : '';
                if (devToken) {
                    const sep = url.includes('?') ? '&' : '?';
                    url = `${url}${sep}token=${encodeURIComponent(devToken)}`;
                }
            }
            if (isHttpsDomain && url.startsWith('http://')) {
                return ''; // Let blobStream handle it via safe blob: URL
            }
            return url;
        }
        if (discoveredStreamUrl) {
            if (isHttpsDomain && discoveredStreamUrl.startsWith('http://')) {
                return ''; // Let blobStream handle it via safe blob: URL
            }
            return withApiOrigin(discoveredStreamUrl, activeApiBase);
        }
        // Never poll /camera/stream on serverless cloud when no stream is published
        if (activeApiBase.includes(':8080') || (typeof window !== 'undefined' && window.location.protocol !== 'https:')) {
            return `${activeApiBase}/camera/stream?t=${streamEpoch}`;
        }
        return '';
    });

    let isLiveStreamPreAnnotated = $derived(
        Boolean(
            activeStreamUrl &&
            (activeStreamUrl.includes('/camera/stream') || activeStreamUrl.includes('/annotated'))
        )
    );

    function handleLiveImgLoaded() {
        streamError = false;
        retryCount = 0;
        mockAnnotatedFrameUrl = '';
        deviceCamAnnotatedUrl = '';
        isLiveFeedActive = true;
        localBackendNotice = '';
        scheduleLiveRefresh();
        startLivePushing();
        if (typeof window !== 'undefined') {
            window.dispatchEvent(new CustomEvent('bawal-camera-online', {
                detail: { active: true, deviceId: 'ESP32S3_441BF6C95678' }
            }));
        }
    }

    function handleLiveImgError() {
        if (!discoveredStreamUrl && typeof window !== 'undefined' && window.location.hostname !== 'localhost' && window.location.protocol !== 'https:') {
            discoveredStreamUrl = 'http://localhost:8080/api/v1/camera/stream';
            streamEpoch = Date.now();
            return;
        }
        streamError = true;
        isLiveFeedActive = false;
        if (isHttpsDomain) {
            localBackendNotice = '⚠️ <strong>Stream Blocked by Browser (Mixed Active Content)</strong>: Your browser blocked <code>' + (discoveredStreamUrl || activeStreamUrl) + '</code> from HTTPS Vercel.<br/><strong>Fix:</strong> Tap the lock icon in your address bar &rarr; <em>Site settings</em> &rarr; <em>Insecure content</em> &rarr; <strong>Allow</strong>, then refresh this page.';
        }
    }

    let isProbingLocal = $state(false);
    let localBackendNotice = $state('');

    async function connectLocalBackendStream() {
        isProbingLocal = true;
        localBackendNotice = 'Probing local backend at http://localhost:8080...';

        const isHttps = typeof window !== 'undefined' && window.location.protocol === 'https:';

        try {
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 1500);
            const res = await fetch('http://localhost:8080/api/v1/health', {
                signal: controller.signal,
                cache: 'no-store'
            }).catch(err => {
                throw err;
            });
            clearTimeout(timeout);

            if (res.ok) {
                setConfiguredApiBase('http://localhost:8080/api/v1');
                discoveredStreamUrl = 'http://localhost:8080/api/v1/camera/stream';
                isAttemptingLocalStream = true;
                streamError = false;
                retryCount = 0;
                streamEpoch = Date.now();
                localBackendNotice = '✓ Local C++ backend connected! Live MJPEG stream and real hardware telemetry active.';
                if (typeof window !== 'undefined') {
                    window.dispatchEvent(new CustomEvent('bawal-backend-updated', {
                        detail: 'http://localhost:8080/api/v1'
                    }));
                }
            } else {
                localBackendNotice = '⚠️ Backend at localhost:8080 responded with an error. Verify bawal_backend is running.';
            }
        } catch {
            if (isHttps) {
                localBackendNotice = '⚠️ <strong>Mixed Content Blocked</strong>: Your browser blocks insecure HTTP connections from an HTTPS domain (Vercel).<br/>' +
                    '<strong>Solutions:</strong><br/>' +
                    '• <strong>📱 Phone Cam Mode</strong>: Click the <strong>Phone Cam</strong> button above to inspect live food with your phone\'s rear camera.<br/>' +
                    '• <strong>Local Network (HTTP)</strong>: Connect phone to laptop Wi-Fi hotspot and open <code>http://&lt;laptop-ip&gt;:5173</code> (runs without mixed content blocking).<br/>' +
                    '• <strong>Browser Setting</strong>: In Chrome/Brave on Android, tap Site Settings &rarr; Insecure content &rarr; Allow.<br/>' +
                    '• <strong>Simulation</strong>: Switch to <strong>Mock Video (Simulation)</strong> for cloud AI inspection.';
            } else {
                localBackendNotice = '⚠️ <strong>Local backend is not active on port 8080</strong>. Start it using: <code>just run-backend</code> or <code>just compose-up bawal_backend</code>.';
            }
        } finally {
            isProbingLocal = false;
        }
    }

    function captureAndPushLiveFrame() {
        if (isPushingLiveFrame) return;

        let sourceEl: CanvasImageSource | null = null;
        if (sourceMode === 'device_cam') {
            if (!deviceCamVideoEl || !isDeviceCamActive || deviceCamVideoEl.readyState < 2) return;
            sourceEl = deviceCamVideoEl;
        } else if (sourceMode === 'live') {
            if (!relayConfigured || !liveImgEl || !liveImgEl.complete || liveImgEl.naturalWidth === 0) return;
            sourceEl = liveImgEl;
        } else {
            return;
        }

        isPushingLiveFrame = true;
        try {
            const canvas = document.createElement('canvas');
            canvas.width = 640;
            canvas.height = 480;
            const ctx = canvas.getContext('2d');
            if (!ctx) {
                isPushingLiveFrame = false;
                return;
            }
            ctx.drawImage(sourceEl, 0, 0, 640, 480);
            canvas.toBlob((blob) => {
                if (!blob) {
                    isPushingLiveFrame = false;
                    return;
                }
                const currentSeq = ++liveFrameSeq;
                const effectiveDeviceId = sourceMode === 'device_cam'
                    ? 'PHONE_CAMERA'
                    : ((deviceId && deviceId !== 'NO_ESP32_NODE' && deviceId !== 'DISCONNECTED') ? deviceId : 'ESP32S3_441BF6C95678');

                fetch(apiEndpoint('/api/v1/camera/frame'), {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'image/jpeg',
                        'X-Device-ID': effectiveDeviceId,
                        'X-Stream-Source': sourceMode === 'device_cam' ? 'DEVICE_CAMERA' : 'LIVE_CAMERA',
                        'X-Frame-ID': String(currentSeq),
                        'X-Inference-Target': 'YOLO_OPENCV4',
                        'X-Return-Annotated': sourceMode === 'device_cam' ? '1' : '0'
                    },
                    body: blob
                })
                .then(async (res) => {
                    if (res && res.ok) {
                        const data = await res.json().catch(() => null);
                        if (data && Array.isArray(data.detections)) {
                            let thumb = '';
                            try {
                                if (data.annotated_image_base64) {
                                    thumb = `data:image/jpeg;base64,${data.annotated_image_base64}`;
                                } else if (canvas && data.detections.length > 0) {
                                    thumb = buildAnnotatedThumbnail(canvas, data.detections);
                                } else if (canvas) {
                                    thumb = canvas.toDataURL('image/jpeg', 0.85);
                                } else if (data.image_path) {
                                    thumb = data.image_path;
                                }
                            } catch {}
                            if (thumb && sourceMode === 'device_cam') {
                                deviceCamAnnotatedUrl = thumb;
                            }

                            if (typeof window !== 'undefined') {
                                window.dispatchEvent(new CustomEvent('bawal-live-detections', {
                                    detail: {
                                        ...data,
                                        source: sourceMode === 'device_cam' ? 'device_camera' : 'live_camera',
                                        deviceId: effectiveDeviceId,
                                        thumbnail: thumb
                                    }
                                }));
                                window.dispatchEvent(new CustomEvent('bawal-camera-online', {
                                    detail: { active: true, deviceId: effectiveDeviceId }
                                }));
                            }
                        }
                    }
                })
                .catch(() => {})
                .finally(() => {
                    isPushingLiveFrame = false;
                });
            }, 'image/jpeg', 0.82);
        } catch {
            isPushingLiveFrame = false;
        }
    }

    function startLivePushing() {
        const intervalMs = sourceMode === 'live' ? 2500 : 1500;
        if (livePushTimer !== null && livePushIntervalMs === intervalMs) return;
        stopLivePushing();
        // Trigger one immediate frame push
        livePushKickoff = setTimeout(() => {
            livePushKickoff = null;
            captureAndPushLiveFrame();
        }, 200);
        livePushIntervalMs = intervalMs;
        livePushTimer = setInterval(() => {
            if ((sourceMode === 'live' && isLiveFeedActive && !streamError && isTabVisible) ||
                (sourceMode === 'device_cam' && isDeviceCamActive && isTabVisible)) {
                captureAndPushLiveFrame();
            }
        }, intervalMs);
    }

    function stopLivePushing() {
        if (livePushKickoff !== null) {
            clearTimeout(livePushKickoff);
            livePushKickoff = null;
        }
        if (livePushTimer) {
            clearInterval(livePushTimer);
            livePushTimer = null;
        }
        livePushIntervalMs = 0;
    }

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
        const refreshDiscoveredStream = async () => {
            syncApiBase();
            const candidates = [
                apiEndpoint('/api/v1/devices/status')
            ];
            if (typeof window !== 'undefined' && window.location.protocol !== 'https:') {
                candidates.unshift('http://localhost:8080/api/v1/devices/status');
            }
            if (typeof window !== 'undefined' && window.location.hostname && window.location.hostname !== 'localhost' && !window.location.hostname.endsWith('.vercel.app') && window.location.protocol !== 'https:') {
                candidates.unshift(`http://${window.location.hostname}:8080/api/v1/devices/status`);
            }

            for (const ep of candidates) {
                try {
                    const c = new AbortController();
                    const to = setTimeout(() => c.abort(), 1200);
                    const res = await fetch(ep, { signal: c.signal, cache: 'no-store' });
                    clearTimeout(to);
                    if (res.ok) {
                        const data = await res.json();
                        const nextUrl = typeof data?.stream_url === 'string' ? data.stream_url : (data?.ip_address ? `http://${data.ip_address}/stream` : '');
                        if (data?.ip_address) {
                            autoDetectedEspIp = data.ip_address;
                        }
                        if (data?.detected_host_ip) {
                            autoDetectedBackendIp = data.detected_host_ip;
                        }
                        if (nextUrl) {
                            if (!discoveredStreamUrl || streamError) {
                                discoveredStreamUrl = nextUrl;
                                if (!manualEspStreamUrl) {
                                    manualEspStreamUrl = nextUrl;
                                }
                                streamError = false;
                            }
                            // Auto-adopt live local backend if current is default
                            const baseCand = ep.replace(/\/devices\/status$/i, '');
                            if (baseCand.startsWith('http') && baseCand !== activeApiBase) {
                                setConfiguredApiBase(baseCand);
                                activeApiBase = baseCand;
                                if (typeof window !== 'undefined') {
                                    window.dispatchEvent(new CustomEvent('bawal-backend-updated', { detail: baseCand }));
                                }
                            }
                            break;
                        }
                    }
                } catch {}
            }
        };
        refreshDiscoveredStream();
        deviceStatusTimer = setInterval(refreshDiscoveredStream, 2000);
        if (typeof document !== 'undefined') {
            document.addEventListener('visibilitychange', handleVisibilityChange);
        }
        handleExternalCameraOnline = (e: any) => {
            const detail = e.detail;
            if (detail?.streamUrl && isValidStreamHost(detail.streamUrl)) {
                manualEspStreamUrl = detail.streamUrl;
                discoveredStreamUrl = detail.streamUrl;
                streamError = false;
                isLiveFeedActive = true;
                streamEpoch = Date.now();
                if (detail?.ip) {
                    autoDetectedEspIp = detail.ip;
                }
            }
        };
        if (typeof window !== 'undefined') {
            window.addEventListener('bawal-camera-online', handleExternalCameraOnline as EventListener);
        }
        if (typeof localStorage !== 'undefined') {
            const savedEspIp = localStorage.getItem('bawal_esp_ip');
            if (savedEspIp && isValidStreamHost(`http://${savedEspIp}`)) {
                autoDetectedEspIp = savedEspIp;
            }
            const savedEspUrl = localStorage.getItem('bawal_esp32_stream_url');
            if (savedEspUrl && isValidStreamHost(savedEspUrl)) {
                manualEspStreamUrl = savedEspUrl;
                // If on HTTPS, do not auto-bind insecure HTTP stream to <img> to avoid Mixed Content errors
                if (!(typeof window !== 'undefined' && window.location.protocol === 'https:' && savedEspUrl.startsWith('http://'))) {
                    if (!discoveredStreamUrl) {
                        discoveredStreamUrl = savedEspUrl;
                    }
                }
            } else if (savedEspUrl && !isValidStreamHost(savedEspUrl)) {
                try { localStorage.removeItem('bawal_esp32_stream_url'); } catch {}
                manualEspStreamUrl = '';
            }
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
                    fetch(apiEndpoint('/api/v1/camera/mode'), {
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
        fetch(apiEndpoint('/api/v1/examples'))
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

        if (sourceMode === 'live') {
            connectLiveWebSocket();
            startLivePushing();
        } else {
            disconnectLiveWebSocket();
            startMockPushing();
        }
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

    function getDetectionBox(det: any): [number, number, number, number] | null {
        if (!det) return null;
        const raw = Array.isArray(det.box) ? det.box : (Array.isArray(det.bbox) ? det.bbox : null);
        let x = raw ? Number(raw[0]) : (typeof det.x === 'number' ? det.x : null);
        let y = raw ? Number(raw[1]) : (typeof det.y === 'number' ? det.y : null);
        let w = raw ? Number(raw[2]) : (typeof det.width === 'number' ? det.width : null);
        let h = raw ? Number(raw[3]) : (typeof det.height === 'number' ? det.height : null);
        if (x === null || y === null || w === null || h === null || isNaN(x) || isNaN(y) || isNaN(w) || isNaN(h) || w <= 0 || h <= 0) {
            return null;
        }
        if (x <= 1 && y <= 1 && w <= 1 && h <= 1 && (w > 0 || h > 0)) {
            x *= 640;
            y *= 480;
            w *= 640;
            h *= 480;
        }
        return [Math.round(x), Math.round(y), Math.round(w), Math.round(h)];
    }

    function formatHudLabel(det: any): { label: string; strokeColor: string; status: string; badgeW: number } {
        const name = det.food_name || det.class_name || 'food_item';
        const confRaw = Number(det.similarity_rate ?? det.confidence) || 0;
        const confPct = Math.round(confRaw <= 1 ? confRaw * 100 : confRaw);
        const status = getItemStatus(det);
        const spoilage = Number(det.spoilage_score) || 0;
        const freshness = Math.max(0, Math.min(100, Math.round(100 - spoilage)));
        const strokeColor = status === 'PASS' ? '#10b981' : (status === 'REVIEW' ? '#f59e0b' : '#ef4444');
        const label = `${name} ${confPct}% [${status} ${freshness}%]`;
        const badgeW = Math.max(88, label.length * 7.5 + 16);
        return { label, strokeColor, status, badgeW };
    }

    function buildAnnotatedThumbnail(canvas: HTMLCanvasElement, detList: any[]): string {
        try {
            const offCanvas = document.createElement('canvas');
            offCanvas.width = canvas.width;
            offCanvas.height = canvas.height;
            const offCtx = offCanvas.getContext('2d');
            if (!offCtx) return canvas.toDataURL('image/jpeg', 0.65);

            offCtx.drawImage(canvas, 0, 0);
            for (const det of detList) {
                const box = getDetectionBox(det);
                if (!box) continue;
                const [bx, by, bw, bh] = box;
                const spoilage = Number(det.spoilage_score) || 0;
                const freshness = Math.max(0, Math.min(100, Math.round(100 - spoilage)));
                const confRaw = Number(det.similarity_rate ?? det.confidence) || 0;
                const confPct = Math.round(confRaw <= 1 ? confRaw * 100 : confRaw);
                const status = getItemStatus(det);
                const strokeColor = status === 'PASS' ? '#10b981' : (status === 'REVIEW' ? '#f59e0b' : '#ef4444');
                const name = det.food_name || det.class_name || 'food_item';
                const label = `${name} ${confPct}% [${status} ${freshness}%]`;

                offCtx.strokeStyle = strokeColor;
                offCtx.lineWidth = 3;
                offCtx.strokeRect(bx, by, bw, bh);

                offCtx.font = 'bold 13px sans-serif';
                const metrics = offCtx.measureText(label);
                const badgeH = 22;
                const badgeW = metrics.width + 12;
                const badgeY = Math.max(0, by - badgeH);
                offCtx.fillStyle = strokeColor;
                offCtx.fillRect(bx, badgeY, badgeW, badgeH);

                offCtx.fillStyle = '#ffffff';
                offCtx.fillText(label, bx + 6, badgeY + 15);
            }
            return offCanvas.toDataURL('image/jpeg', 0.85);
        } catch {
            return canvas.toDataURL('image/jpeg', 0.65);
        }
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

                fetch(apiEndpoint('/api/v1/camera/frame'), {
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
                        'X-Video-Rewind': isRewound ? '1' : '0',
                        'X-Return-Annotated': '1'
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
                                if (data.annotated_image_base64) {
                                    thumb = `data:image/jpeg;base64,${data.annotated_image_base64}`;
                                } else if (mockCanvasEl && data.detections.length > 0) {
                                    thumb = buildAnnotatedThumbnail(mockCanvasEl, data.detections);
                                } else if (mockCanvasEl) {
                                    thumb = mockCanvasEl.toDataURL('image/jpeg', 0.65);
                                } else if (data.image_path) {
                                    thumb = data.image_path;
                                }
                            } catch {}
                            if (thumb) {
                                mockAnnotatedFrameUrl = thumb;
                            }
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
                    } else {
                        streamError = true;
                    }
                })
                .catch(() => {
                    streamError = true;
                })
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
                const currentSeq = ++mockFrameSeq;

                fetch(apiEndpoint('/api/v1/camera/frame'), {
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
                        'X-Video-Rewind': isRewound ? '1' : '0',
                        'X-Return-Annotated': '1'
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
                                if (data.annotated_image_base64) {
                                    thumb = `data:image/jpeg;base64,${data.annotated_image_base64}`;
                                } else if (mockCanvasEl && data.detections.length > 0) {
                                    thumb = buildAnnotatedThumbnail(mockCanvasEl, data.detections);
                                } else if (mockCanvasEl) {
                                    thumb = mockCanvasEl.toDataURL('image/jpeg', 0.65);
                                } else if (data.image_path) {
                                    thumb = data.image_path;
                                }
                            } catch {}
                            if (thumb) {
                                mockAnnotatedFrameUrl = thumb;
                            }
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
                    } else {
                        streamError = true;
                    }
                }).catch(() => {
                    streamError = true;
                });
            }, 'image/jpeg', 0.85);
        } catch {}
    }

    function startMockPushing() {
        stopMockPushing();
        stopLivePushing();
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

    async function postCameraMode(payload: Record<string, unknown>): Promise<void> {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), 1200);
        try {
            await fetch(apiEndpoint('/api/v1/camera/mode'), {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload),
                signal: controller.signal
            });
        } catch {
            // Ignore backend timeout or network error (backend may be offline)
        } finally {
            clearTimeout(timer);
        }
    }

    async function setSourceMode(mode: SourceModeType) {
        if (sourceMode === mode && !isSwitchingMode) return;
        isSwitchingMode = true;
        const prevMode = sourceMode;
        sourceMode = mode;
        if (typeof localStorage !== 'undefined') {
            try { localStorage.setItem('bawal_source_mode', mode); } catch {}
        }
        streamError = false;
        retryCount = 0;
        mockAnnotatedFrameUrl = '';
        deviceCamAnnotatedUrl = '';
        if (typeof window !== 'undefined') {
            // Wipe HUD boxes immediately; the backend also clears its snapshot.
            window.dispatchEvent(new CustomEvent('bawal-clear-detections'));
        }

        // Clean up previous mode
        if (prevMode === 'mock') {
            stopMockPushing();
            if (mockVideoEl) {
                mockVideoEl.pause();
            }
        } else if (prevMode === 'device_cam') {
            stopDeviceCam();
            stopLivePushing();
        } else {
            stopLivePushing();
        }

        // Safety fallback timer: scrim MUST NOT remain stuck on screen under any circumstance
        const safetyTimer = setTimeout(() => {
            isSwitchingMode = false;
        }, 1200);

        try {
            if (mode === 'mock') {
                disconnectLiveWebSocket();
                // 1. Immediately request backend mode change and wipe frames/detections
                await postCameraMode({
                    mode: 'mock',
                    video_source: mockFileName,
                    video_uuid: mockVideoUUID
                });

                // 2. Allow Svelte 5 DOM reconciliation of <video> element
                await tick();
                await new Promise(r => setTimeout(r, 200));

                if (mockVideoEl) {
                    mockVideoEl.muted = true;
                    try {
                        await mockVideoEl.play();
                    } catch {}
                }
                startMockPushing();
            } else if (mode === 'device_cam') {
                disconnectLiveWebSocket();
                await postCameraMode({ mode: 'live' });
                await new Promise(r => setTimeout(r, 300));
                await startDeviceCam();
            } else {
                await postCameraMode({ mode: 'live' });
                await new Promise(r => setTimeout(r, 300));
                streamEpoch = Date.now();
                connectLiveWebSocket();
                startLivePushing();
            }
        } catch (err) {
            console.warn('[CameraStream] Mode switch transition error:', err);
        } finally {
            clearTimeout(safetyTimer);
            isSwitchingMode = false;
        }
    }

    function resetRewindState() {
        isRewound = false;
        lastVideoTime = 0;
        fetch(apiEndpoint('/api/v1/devices/mock/reset'), {
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
        const safetyTimer = setTimeout(() => {
            isSwitchingMode = false;
        }, 1200);

        if (typeof localStorage !== 'undefined') {
            try { localStorage.setItem('bawal_mock_selected_video', name); } catch {}
        }
        const nextUUID = videoUUIDMap[name] || computeMockVideoUUID(name);

        try {
            await postCameraMode({
                mode: 'mock',
                video_source: name,
                video_uuid: nextUUID
            });
            if (mockVideoSrc.startsWith('blob:')) {
                URL.revokeObjectURL(mockVideoSrc);
            }
            mockVideoSrc = `/examples/${name}`;
            streamError = false;
            streamEpoch = Date.now();

            await tick();
            await new Promise(r => setTimeout(r, 200));
            if (mockVideoEl) {
                mockVideoEl.currentTime = 0;
                mockVideoEl.muted = true;
                await mockVideoEl.play().catch(() => {});
                pushCurrentFrameOnce();
            }
        } catch (err) {
            console.warn('[CameraStream] selectExample transition error:', err);
        } finally {
            clearTimeout(safetyTimer);
            isSwitchingMode = false;
        }
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
            postCameraMode({
                mode: 'mock',
                video_source: file.name,
                video_uuid: nextUUID
            });
            if (mockVideoSrc.startsWith('blob:')) {
                URL.revokeObjectURL(mockVideoSrc);
            }
            mockVideoSrc = URL.createObjectURL(file);
            streamError = false;
            streamEpoch = Date.now();
            setTimeout(() => {
                if (mockVideoEl) {
                    mockVideoEl.currentTime = 0;
                    mockVideoEl.muted = true;
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
                mockVideoEl.muted = true;
                mockVideoEl.play().catch(() => {});
                startMockPushing();
                setTimeout(pushCurrentFrameOnce, 50);
            }
        }
    }

    function handleVideoCanPlay() {
        if (mockVideoEl && sourceMode === 'mock') {
            if (mockDuration === 0 && mockVideoEl.duration) {
                mockDuration = mockVideoEl.duration;
            }
            if (mockVideoEl.paused) {
                mockVideoEl.muted = true;
                mockVideoEl.play().catch(() => {});
            }
            startMockPushing();
        }
    }

    function handleVideoError(e: Event) {
        console.warn('[CameraStream] Mock video element playback error:', e);
        isSwitchingMode = false;
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
        stopLivePushing();
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
            if (handleExternalCameraOnline) {
                window.removeEventListener('bawal-camera-online', handleExternalCameraOnline as EventListener);
            }
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
        stopLivePushing();
        stopDeviceCam();
        disconnectLiveWebSocket();
        stopBlobStream();
        if (mockVideoSrc.startsWith('blob:')) {
            URL.revokeObjectURL(mockVideoSrc);
        }
    });

    $effect(() => {
        if (hasCamera) {
            streamError = false;
        }
    });

    // Reconnect when the dashboard tab becomes active after initial mount.
    // The live view stays mounted on other tabs, so relying on onMount alone
    // leaves the relay WebSocket disconnected until a manual mode switch.
    $effect(() => {
        if (typeof window === 'undefined') return;
        const shouldStream = isActive && isTabVisible && sourceMode === 'live';
        if (shouldStream) {
            connectLiveWebSocket();
        } else {
            disconnectLiveWebSocket();
            isLiveFeedActive = false;
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
                <span class="live-dot" class:offline={sourceMode === 'live' ? (!isLiveFeedActive || streamError) : (sourceMode === 'device_cam' ? (!isDeviceCamActive || !!deviceCamError) : false)}></span>
                <strong>
                    {#if sourceMode === 'live'}
                        Live ESP32 Inspection View
                    {:else if sourceMode === 'device_cam'}
                        Phone / Device Camera (Cloud AI)
                    {:else}
                        Mock Video Inspection View (AI Connected)
                    {/if}
                </strong>
            </div>

            <!-- Video / Camera Ingestion Source Mode Switcher -->
            <div class="source-mode-switcher">
                <button
                    type="button"
                    class="mode-pill-btn"
                    class:active={sourceMode === 'live'}
                    onclick={() => setSourceMode('live')}
                    title="Live continuous stream push from ESP32-S3 camera"
                >
                    <span class="pill-icon">📹</span> ESP32 Stream
                </button>
                <button
                    type="button"
                    class="mode-pill-btn"
                    class:active={sourceMode === 'device_cam'}
                    onclick={() => setSourceMode('device_cam')}
                    title="Direct live food inspection using your smartphone rear camera or webcam"
                >
                    <span class="pill-icon">📱</span> Phone / Laptop Cam
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

            <span class="pill-badge" class:badge-active={sourceMode === 'mock' || sourceMode === 'device_cam' || isLiveFeedActive || (hasCamera && !streamError)}>
                {#if sourceMode === 'live'}
                    {isLiveFeedActive || (hasCamera && !streamError) ? 'Live Push (Active HUD)' : 'Standby'}
                {:else if sourceMode === 'device_cam'}
                    {isDeviceCamActive ? '📱 Mobile Cam Active (1.5s YOLO)' : (deviceCamError ? 'Camera Error' : 'Requesting Access')}
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
                <!-- Smooth Native 60 FPS Mock Video Ingestion Player -->
                <video
                    bind:this={mockVideoEl}
                    src={resolvedMockVideoSrc}
                    aria-label="Mock Video Feed"
                    class="feed-video"
                    crossorigin="anonymous"
                    playsinline
                    muted
                    autoplay
                    preload="auto"
                    loop
                    onloadedmetadata={handleVideoLoaded}
                    oncanplay={handleVideoCanPlay}
                    ontimeupdate={handleVideoTimeUpdate}
                    onplay={() => (mockIsPlaying = true)}
                    onpause={() => (mockIsPlaying = false)}
                    onerror={handleVideoError}
                ></video>

                <!-- Dynamic AI Bounding Box HUD Overlay for Mock Video (Active ONLY during playback with detections) -->
                {#if !isSwitchingMode && mockIsPlaying && detections && detections.length > 0}
                    <svg class="hud-overlay" viewBox="0 0 640 480" preserveAspectRatio="xMidYMid meet">
                        {#each detections as det}
                            {@const box = getDetectionBox(det)}
                            {#if box}
                                {@const hud = formatHudLabel(det)}
                                {@const [bx, by, bw, bh] = box}
                                <rect
                                    x={bx}
                                    y={by}
                                    width={bw}
                                    height={bh}
                                    fill="none"
                                    stroke={hud.strokeColor}
                                    stroke-width="2.5"
                                    rx="4"
                                />
                                <rect
                                    x={bx}
                                    y={Math.max(0, by - 20)}
                                    width={hud.badgeW}
                                    height="20"
                                    fill={hud.strokeColor}
                                    rx="3"
                                />
                                <text
                                    x={bx + 6}
                                    y={Math.max(14, by - 6)}
                                    fill="#ffffff"
                                    font-size="11"
                                    font-weight="bold"
                                    font-family="sans-serif"
                                >
                                    {hud.label}
                                </text>
                            {/if}
                        {/each}
                    </svg>
                {/if}
            {:else if sourceMode === 'device_cam'}
                <!-- Direct Smartphone / Webcam Video Feed (Always mounted so element reference is bound) -->
                <video
                    bind:this={deviceCamVideoEl}
                    aria-label="Device Camera Stream"
                    class="feed-video"
                    class:hidden-video={!isDeviceCamActive}
                    playsinline
                    autoplay
                    muted
                ></video>

                {#if isDeviceCamActive}
                    <!-- Dynamic AI Bounding Box HUD Overlay for Device Cam (Active ONLY when camera is running with detections) -->
                    {#if !isSwitchingMode && detections && detections.length > 0}
                        <svg class="hud-overlay" viewBox="0 0 640 480" preserveAspectRatio="xMidYMid meet">
                            {#each detections as det}
                                {@const box = getDetectionBox(det)}
                                {#if box}
                                    {@const hud = formatHudLabel(det)}
                                    {@const [bx, by, bw, bh] = box}
                                    <rect
                                        x={bx}
                                        y={by}
                                        width={bw}
                                        height={bh}
                                        fill="none"
                                        stroke={hud.strokeColor}
                                        stroke-width="2.5"
                                        rx="4"
                                    />
                                    <rect
                                        x={bx}
                                        y={Math.max(0, by - 20)}
                                        width={hud.badgeW}
                                        height="20"
                                        fill={hud.strokeColor}
                                        rx="3"
                                    />
                                    <text
                                        x={bx + 6}
                                        y={Math.max(14, by - 6)}
                                        fill="#ffffff"
                                        font-size="11"
                                        font-weight="bold"
                                        font-family="sans-serif"
                                    >
                                        {hud.label}
                                    </text>
                                {/if}
                            {/each}
                        </svg>
                    {/if}
                {:else}
                    <!-- Device Cam Standby / Permission Notice -->
                    <div class="standby-box">
                        <div class="standby-radar-ring">
                            <span class="standby-icon">📱</span>
                        </div>
                        <h4>Smartphone / Web Camera</h4>
                        <p>{deviceCamError || 'Point your smartphone camera or laptop webcam at a food meal tray. Real-time frames are captured and inspected by YOLO AI in real-time.'}</p>
                        <div class="standby-actions-row">
                            <button type="button" class="switch-mock-btn" onclick={startDeviceCam}>
                                📷 Start Camera Feed
                            </button>
                            <button type="button" class="retry-btn" onclick={() => setSourceMode('live')}>
                                📹 Switch to ESP32
                            </button>
                        </div>
                    </div>
                {/if}
            {:else}
                <!-- Live Stream Mode (Unified WebSocket / MJPEG Ingress from ESP32-S3 Camera via Backend OpenCV) -->
                {#if Boolean(wsLiveFrameUrl || activeStreamUrl) && (hasCamera || isAttemptingLocalStream || discoveredStreamUrl || wsLiveFrameUrl) && !streamError}
                    <img
                        bind:this={liveImgEl}
                        src={wsLiveFrameUrl || activeStreamUrl}
                        alt="AI Live Inspection Stream"
                        class="feed-video"
                        crossorigin="anonymous"
                        onload={handleLiveImgLoaded}
                        onerror={handleLiveImgError}
                    />

                    <!-- Dynamic AI Bounding Box HUD Overlay for Live ESP Stream (Active ONLY when streaming with detections) -->
                    {#if !isLiveStreamPreAnnotated && !isSwitchingMode && isLiveFeedActive && !streamError && detections && detections.length > 0}
                        <svg class="hud-overlay" viewBox="0 0 640 480" preserveAspectRatio="xMidYMid meet">
                            {#each detections as det}
                                {@const box = getDetectionBox(det)}
                                {#if box}
                                    {@const hud = formatHudLabel(det)}
                                    {@const [bx, by, bw, bh] = box}
                                    <rect
                                        x={bx}
                                        y={by}
                                        width={bw}
                                        height={bh}
                                        fill="none"
                                        stroke={hud.strokeColor}
                                        stroke-width="2.5"
                                        rx="4"
                                    />
                                    <rect
                                        x={bx}
                                        y={Math.max(0, by - 20)}
                                        width={hud.badgeW}
                                        height="20"
                                        fill={hud.strokeColor}
                                        rx="3"
                                    />
                                    <text
                                        x={bx + 6}
                                        y={Math.max(14, by - 6)}
                                        fill="#ffffff"
                                        font-size="11"
                                        font-weight="bold"
                                        font-family="sans-serif"
                                    >
                                        {hud.label}
                                    </text>
                                {/if}
                            {/each}
                        </svg>
                    {/if}
                {:else}
                    <!-- Standby State -->
                    <div class="standby-box">
                        <div class="standby-radar-ring">
                            <span class="standby-icon">📷</span>
                        </div>
                        <h4>ESP32-S3 Live Stream Awaiting Signal</h4>
                        <p>No active MJPEG stream received on <code>/api/v1/camera/stream</code>. The physical camera node is either in standby or on a local subnet.</p>

                        {#if localBackendNotice}
                            <div class="local-backend-alert">
                                <span>{@html localBackendNotice}</span>
                            </div>
                        {/if}

                        {#if autoDetectedEspIp || autoDetectedBackendIp}
                            <div class="auto-detected-row">
                                <span class="auto-detected-label">⚡ Auto-Discovered:</span>
                                {#if autoDetectedEspIp}
                                    <button
                                        type="button"
                                        class="auto-detected-pill"
                                        onclick={() => {
                                            manualEspStreamUrl = `http://${autoDetectedEspIp}/stream`;
                                            connectManualEspStream();
                                        }}
                                        title="Click to auto-connect ESP32 camera without typing IP"
                                    >
                                        📹 ESP32 Camera ({autoDetectedEspIp}) &rarr; Connect
                                    </button>
                                {/if}
                                {#if autoDetectedBackendIp}
                                    <button
                                        type="button"
                                        class="auto-detected-pill backend"
                                        onclick={() => {
                                            manualEspStreamUrl = `http://${autoDetectedBackendIp}:8080/api/v1/camera/stream`;
                                            connectManualEspStream();
                                        }}
                                        title="Click to sync with Host C++ Backend"
                                    >
                                        🖥️ Host Backend ({autoDetectedBackendIp}:8080) &rarr; Connect
                                    </button>
                                {/if}
                            </div>
                        {/if}

                        <div class="esp-ip-connect-bar">
                            <input
                                type="text"
                                bind:value={manualEspStreamUrl}
                                placeholder="Enter ESP32 IP (e.g. 192.168.43.50 or http://192.168.43.50/stream)"
                                class="esp-ip-input"
                            />
                            <button type="button" class="esp-ip-btn" onclick={connectManualEspStream}>
                                ⚡ Connect ESP IP
                            </button>
                        </div>

                        {#if isHttpsDomain}
                            <div class="bypass-card">
                                <h5>💡 Viewing Local ESP32 Stream on HTTPS Vercel (Mobile Chrome / Brave):</h5>
                                <ol>
                                    <li>Tap the <strong>Site Settings icon (tune / lock)</strong> beside <code>bawal-vision.vercel.app</code> in your phone address bar.</li>
                                    <li>Tap <strong>Site settings</strong> &rarr; <strong>Insecure content</strong> &rarr; change to <strong>Allow</strong>.</li>
                                    <li>Refresh the page. The ESP32 camera stream will load immediately without browser blocking!</li>
                                </ol>
                            </div>
                        {/if}

                        <div class="standby-actions-row">
                            <button type="button" class="switch-mock-btn" onclick={() => setSourceMode('device_cam')}>
                                📱 Use Phone Camera
                            </button>
                            <button type="button" class="switch-mock-btn" onclick={() => { setSourceMode('mock'); togglePlayPause(); }}>
                                🎬 Switch to Mock Video (Simulation)
                            </button>
                            <button type="button" class="local-backend-btn" onclick={connectLocalBackendStream} disabled={isProbingLocal}>
                                {#if isProbingLocal}
                                    <span class="btn-spinner"></span> Checking :8080...
                                {:else}
                                    🔌 Connect Localhost Backend (:8080)
                                {/if}
                            </button>
                            <button type="button" class="retry-btn" onclick={() => { streamError = false; retryCount = 0; streamEpoch = Date.now(); localBackendNotice = ''; }}>
                                ↻ Reconnect Camera
                            </button>
                        </div>
                    </div>
                {/if}
            {/if}

            {#if isSwitchingMode}
                <!-- svelte-ignore a11y_click_events_have_key_events -->
                <!-- svelte-ignore a11y_no_static_element_interactions -->
                <div class="mode-switching-scrim" onclick={() => (isSwitchingMode = false)}>
                    <div class="switching-spinner"></div>
                    <div class="switching-status-text">
                        <strong>Switching to {sourceMode === 'mock' ? 'Mock Simulation' : (sourceMode === 'device_cam' ? 'Phone / Device Camera' : 'Live ESP32-S3')}</strong>
                        <span>Synchronizing video pipeline & clearing detection overlays...</span>
                    </div>
                    <button type="button" class="scrim-dismiss-btn" onclick={(e) => { e.stopPropagation(); isSwitchingMode = false; }}>
                        ✕ Dismiss
                    </button>
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
                <span class="device-label">📷 Device: {sourceMode === 'mock' ? `MOCK_VIDEO_CAM (UUID: ${mockVideoUUID.slice(0, 8)})` : (sourceMode === 'device_cam' ? 'PHONE_CAMERA (Cloud AI)' : (deviceId || 'ESP32S3_CAM'))}</span>
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

    .hud-overlay {
        position: absolute;
        top: 0;
        left: 0;
        width: 100%;
        height: 100%;
        pointer-events: none;
        z-index: 5;
    }

    .feed-video {
        width: 100%;
        height: 100%;
        object-fit: contain;
        display: block;
        margin: auto;
    }

    .feed-video.hidden-video {
        display: none !important;
    }

    .auto-detected-row {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        justify-content: center;
        margin-top: 10px;
        margin-bottom: 4px;
    }

    .auto-detected-label {
        font-size: 0.78rem;
        font-weight: 600;
        color: #f59e0b;
    }

    .auto-detected-pill {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        padding: 5px 12px;
        border-radius: 9999px;
        font-size: 0.78rem;
        font-weight: 600;
        background: rgba(16, 185, 129, 0.15);
        color: #10b981;
        border: 1px solid rgba(16, 185, 129, 0.4);
        cursor: pointer;
        transition: all 0.2s ease;
    }

    .auto-detected-pill:hover {
        background: rgba(16, 185, 129, 0.3);
        transform: translateY(-1px);
    }

    .auto-detected-pill.backend {
        background: rgba(56, 189, 248, 0.15);
        color: #38bdf8;
        border-color: rgba(56, 189, 248, 0.4);
    }

    .auto-detected-pill.backend:hover {
        background: rgba(56, 189, 248, 0.3);
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

    .standby-radar-ring {
        display: inline-flex;
        align-items: center;
        justify-content: center;
        width: 56px;
        height: 56px;
        border-radius: 50%;
        background: rgba(56, 189, 248, 0.1);
        border: 1px solid rgba(56, 189, 248, 0.25);
        animation: radarPulse 2.4s infinite ease-in-out;
    }

    @keyframes radarPulse {
        0%, 100% { transform: scale(1); opacity: 0.9; }
        50% { transform: scale(1.08); opacity: 1; }
    }

    .standby-actions-row {
        display: flex;
        align-items: center;
        gap: 10px;
        margin-top: 10px;
        flex-wrap: wrap;
        justify-content: center;
    }

    .switch-mock-btn {
        background: var(--accent-emerald, #10b981);
        color: #ffffff;
        font-weight: 700;
        font-size: 0.78rem;
        border: none;
        padding: 8px 16px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        display: inline-flex;
        align-items: center;
        gap: 6px;
        transition: transform 0.15s ease, filter 0.15s ease;
        box-shadow: 0 2px 8px rgba(16, 185, 129, 0.3);
    }

    .switch-mock-btn:hover {
        transform: translateY(-1px);
        filter: brightness(1.1);
    }

    .local-backend-btn {
        background: #3b82f6;
        color: #ffffff;
        font-weight: 700;
        font-size: 0.78rem;
        border: none;
        padding: 8px 16px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        display: inline-flex;
        align-items: center;
        gap: 6px;
        transition: transform 0.15s ease, filter 0.15s ease;
        box-shadow: 0 2px 8px rgba(59, 130, 246, 0.3);
    }

    .local-backend-btn:hover {
        transform: translateY(-1px);
        filter: brightness(1.1);
    }

    .local-backend-alert {
        background: rgba(239, 68, 68, 0.12);
        border: 1px solid rgba(239, 68, 68, 0.35);
        color: #fca5a5;
        padding: 10px 14px;
        border-radius: 8px;
        font-size: 0.78rem;
        line-height: 1.4;
        max-width: 520px;
        margin: 10px auto 4px auto;
        text-align: center;
    }

    .local-backend-alert a {
        color: #93c5fd;
        text-decoration: underline;
    }

    .esp-ip-connect-bar {
        display: flex;
        align-items: center;
        gap: 8px;
        width: 100%;
        max-width: 520px;
        margin: 14px auto 4px auto;
    }

    .esp-ip-input {
        flex: 1;
        background: rgba(15, 23, 42, 0.7);
        border: 1px solid rgba(255, 255, 255, 0.22);
        color: #ffffff;
        padding: 9px 12px;
        font-size: 0.8rem;
        border-radius: var(--radius-sm);
        outline: none;
        transition: border-color 0.15s ease;
    }

    .esp-ip-input:focus {
        border-color: #38bdf8;
    }

    .esp-ip-btn {
        background: #0284c7;
        color: #ffffff;
        font-weight: 700;
        font-size: 0.78rem;
        border: none;
        padding: 9px 16px;
        border-radius: var(--radius-sm);
        cursor: pointer;
        white-space: nowrap;
        transition: background 0.15s ease, transform 0.15s ease;
    }

    .esp-ip-btn:hover {
        background: #0369a1;
        transform: translateY(-1px);
    }

    .bypass-card {
        background: rgba(15, 23, 42, 0.9);
        border: 1px solid rgba(56, 189, 248, 0.35);
        border-radius: var(--radius-sm);
        padding: 12px 16px;
        margin: 10px auto 6px auto;
        max-width: 520px;
        text-align: left;
        font-size: 0.76rem;
        color: #e2e8f0;
    }

    .bypass-card h5 {
        margin: 0 0 6px 0;
        color: #38bdf8;
        font-size: 0.8rem;
        font-weight: 700;
    }

    .bypass-card ol {
        margin: 0;
        padding-left: 20px;
        line-height: 1.55;
    }

    .retry-btn {
        background: rgba(255, 255, 255, 0.08);
        color: var(--text-primary);
        font-weight: 600;
        font-size: 0.75rem;
        border: 1px solid var(--border-card);
        padding: 7px 14px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        transition: background 0.15s ease;
    }

    .retry-btn:hover {
        background: rgba(255, 255, 255, 0.14);
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

    .scrim-dismiss-btn {
        margin-top: 8px;
        padding: 5px 14px;
        font-size: 0.75rem;
        font-weight: 500;
        background: rgba(255, 255, 255, 0.12);
        color: #e2e8f0;
        border: 1px solid rgba(255, 255, 255, 0.2);
        border-radius: 6px;
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .scrim-dismiss-btn:hover {
        background: rgba(255, 255, 255, 0.22);
        color: #ffffff;
    }

    @keyframes fadeInScrim {
        from { opacity: 0; }
        to { opacity: 1; }
    }

    @keyframes spinScrim {
        to { transform: rotate(360deg); }
    }
</style>
