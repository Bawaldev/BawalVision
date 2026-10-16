<script lang="ts">
    import { onDestroy } from 'svelte';
    import type { FoodDetection } from '../types';

    interface Props {
        detections: FoodDetection[];
        deviceId?: string;
        isLive?: boolean;
        hasCamera?: boolean;
        streamUrl?: string;
    }

    let {
        detections = [],
        deviceId = '',
        isLive = true,
        hasCamera = false,
        streamUrl = ''
    }: Props = $props();

    const standardFoodItems = [
        { id: 'rice', aliases: ['rice', 'nasi'], label: 'Rice (Nasi)', icon: '🍚' },
        { id: 'chicken', aliases: ['chicken', 'ayam'], label: 'Chicken (Ayam)', icon: '🍗' },
        { id: 'egg', aliases: ['egg', 'telur'], label: 'Egg (Telur)', icon: '🥚' },
        { id: 'tempeh', aliases: ['tempeh', 'tempe'], label: 'Tempeh (Tempe)', icon: '🧇' },
        { id: 'tofu', aliases: ['tofu', 'tahu'], label: 'Tofu (Tahu)', icon: '🧊' },
        { id: 'banana', aliases: ['banana', 'pisang'], label: 'Banana (Pisang)', icon: '🍌' },
        { id: 'orange', aliases: ['orange', 'jeruk'], label: 'Orange (Jeruk)', icon: '🍊' },
        { id: 'spinach', aliases: ['spinach', 'bayam'], label: 'Spinach (Bayam)', icon: '🥬' },
        { id: 'water_spinach', aliases: ['water_spinach', 'waterspinach', 'kangkung'], label: 'Water Spinach (Kangkung)', icon: '🌱' }
    ];

    type SourceModeType = 'live' | 'mock';
    let sourceMode = $state<SourceModeType>('live');

    let exampleVideos = $state<string[]>([
        'sample_tray.mp4',
        'sample_conveyor.mp4',
        'fish_freshness_eval.mp4'
    ]);
    let selectedVideo = $state('sample_tray.mp4');
    let mockVideoSrc = $state('/examples/sample_tray.mp4');
    let mockFileName = $state('sample_tray.mp4');
    let isCustomUpload = $state(false);
    let fileInputRef = $state<HTMLInputElement | null>(null);

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
    let mockFrameSeq = 0;

    let streamError = $state(false);
    let streamEpoch = $state(Date.now());
    let retryCount = $state(0);
    const MAX_RETRIES = 3;

    let activeStreamUrl = $derived(
        streamUrl || `/api/v1/camera/stream?t=${streamEpoch}`
    );

    // Fetch list of examples from backend /examples directory (lightweight & safe)
    $effect(() => {
        fetch('/api/v1/examples')
            .then(res => res.ok ? res.json() : null)
            .then(data => {
                if (data && Array.isArray(data.videos) && data.videos.length > 0) {
                    const merged = Array.from(new Set([...data.videos, ...exampleVideos]));
                    exampleVideos = merged;
                }
            })
            .catch(() => {
                // Fallback to presets if backend is unavailable
            });
    });

    function captureAndPushFrame() {
        if (!mockVideoEl || !mockCanvasEl || isPushingFrame) return;
        if (mockVideoEl.paused || mockVideoEl.ended || mockVideoEl.readyState < 2) return;

        const ctx = mockCanvasEl.getContext('2d');
        if (!ctx) return;

        isPushingFrame = true;
        try {
            ctx.drawImage(mockVideoEl, 0, 0, mockCanvasEl.width, mockCanvasEl.height);
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
                        'X-Frame-ID': String(currentSeq)
                    },
                    body: blob
                })
                .then((res) => {
                    if (res.ok) {
                        mockPushedFrames++;
                        streamError = false;
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
            ctx.drawImage(mockVideoEl, 0, 0, mockCanvasEl.width, mockCanvasEl.height);
            mockCanvasEl.toBlob((blob) => {
                if (!blob) return;
                fetch('/api/v1/camera/frame', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'image/jpeg',
                        'X-Device-ID': 'MOCK_VIDEO_CAM',
                        'X-Frame-ID': String(++mockFrameSeq)
                    },
                    body: blob
                }).then(() => {
                    mockPushedFrames++;
                    streamError = false;
                }).catch(() => {});
            }, 'image/jpeg', 0.85);
        } catch {}
    }

    function startMockPushing() {
        stopMockPushing();
        // Run continuous ~16 FPS frame capture into AI vision pipeline
        pushInterval = setInterval(() => {
            if (sourceMode === 'mock') {
                captureAndPushFrame();
            }
        }, 60);
    }

    function stopMockPushing() {
        if (pushInterval) {
            clearInterval(pushInterval);
            pushInterval = null;
        }
    }

    function setSourceMode(mode: SourceModeType) {
        sourceMode = mode;
        if (mode === 'mock') {
            streamError = false;
            streamEpoch = Date.now();
            setTimeout(() => {
                if (mockVideoEl) {
                    mockVideoEl.play().catch(() => {});
                }
                startMockPushing();
            }, 60);
        } else {
            stopMockPushing();
            if (mockVideoEl) {
                mockVideoEl.pause();
            }
            // Disconnect MOCK_VIDEO_CAM so live camera immediately becomes the active device
            fetch('/api/v1/devices/disconnect', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ device_id: 'MOCK_VIDEO_CAM' })
            }).catch(() => {});
            retryCount = 0;
            streamEpoch = Date.now();
        }
    }

    function selectExample(name: string) {
        selectedVideo = name;
        mockFileName = name;
        isCustomUpload = false;
        if (mockVideoSrc.startsWith('blob:')) {
            URL.revokeObjectURL(mockVideoSrc);
        }
        mockVideoSrc = `/examples/${name}`;
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

    function handleFileUpload(event: Event) {
        const input = event.target as HTMLInputElement;
        if (input.files && input.files[0]) {
            const file = input.files[0];
            mockFileName = file.name;
            selectedVideo = 'custom';
            isCustomUpload = true;
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
            if (sourceMode === 'mock') {
                mockVideoEl.play().catch(() => {});
                startMockPushing();
                setTimeout(pushCurrentFrameOnce, 50);
            }
        }
    }

    function handleVideoTimeUpdate() {
        if (mockVideoEl) {
            mockCurrentTime = mockVideoEl.currentTime;
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
            mockVideoEl.currentTime = val;
            mockCurrentTime = val;
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
            <!-- Hidden Offscreen Video & Canvas for Mock Video AI Frame Ingestion -->
            <video
                bind:this={mockVideoEl}
                src={mockVideoSrc}
                crossorigin="anonymous"
                playsinline
                muted
                loop
                style="display: none;"
                onloadedmetadata={handleVideoLoaded}
                ontimeupdate={handleVideoTimeUpdate}
                onplay={() => (mockIsPlaying = true)}
                onpause={() => (mockIsPlaying = false)}
            ></video>
            <canvas
                bind:this={mockCanvasEl}
                width="640"
                height="480"
                style="display: none;"
            ></canvas>

            {#if (hasCamera || sourceMode === 'mock') && !streamError}
                <!-- Unified MJPEG Stream: Live or Mock frames processed identically by C++ YOLOv8 & Real-ESRGAN -->
                <img
                    src={activeStreamUrl}
                    alt="AI Inspection Stream"
                    class="feed-video"
                    onload={() => { streamError = false; retryCount = 0; }}
                    onerror={() => {
                        if (sourceMode === 'live') streamError = true;
                    }}
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
                                <option value={ex}>{ex}</option>
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

                    <div class="ai-status-indicator">
                        <span class="ai-dot-pulse"></span>
                        <span>AI Pipeline: {mockIsPlaying ? 'Active Ingest' : 'Paused'} ({mockPushedFrames} frames)</span>
                    </div>
                </div>
            </div>
        {/if}

        <div class="viewport-footer">
            <span>📷 Device: {sourceMode === 'mock' ? 'MOCK_VIDEO_CAM' : (deviceId || 'ESP32S3_CAM')}</span>
            <span>
                {#if sourceMode === 'live'}
                    ⚡ Ingestion: MJPEG Push (/api/v1/camera/stream)
                {:else}
                    ⚡ Ingestion: MP4 Frame Ingestion -> ONNX YOLOv8 & Super-Resolution ({mockFileName})
                {/if}
            </span>
        </div>
    </div>

    <!-- Detected Items Live Panel -->
    <div class="entities-panel">
        <div class="panel-header">
            <div class="panel-title-group">
                <span class="entity-group-icon">🍱</span>
                <h3>Detected Items</h3>
            </div>
            <span class="active-count" class:has-items={detections.length > 0}>
                {detections.length} {detections.length === 1 ? 'Item' : 'Items'} Active
            </span>
        </div>

        <div class="entity-chips-grid">
            {#if detections.length === 0}
                <div class="empty-tray-state">
                    <span class="empty-icon">🍽️</span>
                    <strong>Awaiting Food Tray</strong>
                    <p>Place meal tray in camera view for real-time AI food detection & quality inspection.</p>
                </div>
            {:else}
                {#each detections as det, idx}
                    {@const spoilage = Number(det.spoilage_score) || 0}
                    {@const freshness = Math.max(0, Math.min(100, Math.round(100 - spoilage)))}
                    {@const status = (det.status as 'PASS' | 'REVIEW' | 'REJECT') || (spoilage >= 50 ? 'REJECT' : (spoilage >= 25 ? 'REVIEW' : 'PASS'))}
                    
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
        grid-template-columns: 1fr 260px;
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
        gap: 6px;
        font-size: 0.7rem;
        font-weight: 600;
        color: #059669;
        margin-left: auto;
        white-space: nowrap;
    }

    .ai-dot-pulse {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        background: #10b981;
        box-shadow: 0 0 6px #10b981;
        animation: pulse-dot 1.5s infinite;
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
        padding: 8px 16px;
        background: #f8fafc;
        border-top: 1px solid var(--border-subtle);
        font-size: 0.75rem;
        gap: 12px;
    }

    .mock-select-group {
        display: flex;
        align-items: center;
        gap: 8px;
        flex: 1;
        min-width: 0;
    }

    .mock-label {
        color: var(--text-secondary);
        white-space: nowrap;
        font-size: 0.72rem;
        font-weight: 600;
    }

    .example-select {
        background: #ffffff;
        color: var(--text-primary);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-sm);
        padding: 4px 10px;
        font-size: 0.75rem;
        font-weight: 600;
        outline: none;
        cursor: pointer;
        max-width: 260px;
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
        padding: 4px 12px;
        font-size: 0.72rem;
        font-weight: 600;
        cursor: pointer;
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
        padding: 10px 16px;
        font-size: 0.72rem;
        color: var(--text-muted);
        background: var(--bg-card);
        border-top: 1px solid var(--border-subtle);
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
</style>
