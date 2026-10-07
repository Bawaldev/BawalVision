<script lang="ts">
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

    let streamError = $state(false);

    let activeStreamUrl = $derived(
        streamUrl || (deviceId ? `/api/v1/camera/stream/${deviceId}` : '/api/v1/camera/stream')
    );

    function isDetected(item: typeof standardFoodItems[number]): { found: boolean; confidence: number } {
        if (!hasCamera || detections.length === 0) {
            return { found: false, confidence: 0 };
        }
        const matched = detections.find(d => {
            const rawName = (d.food_name || '').toLowerCase().replace(/[\s_-]+/g, '');
            return item.aliases.some(alias => {
                const cleanAlias = alias.toLowerCase().replace(/[\s_-]+/g, '');
                return rawName.includes(cleanAlias) || cleanAlias.includes(rawName);
            });
        });
        return { found: !!matched, confidence: matched ? matched.similarity_rate : 0 };
    }

    function getBoxColor(det: FoodDetection): string {
        if (det.status === 'REJECT' || (det.spoilage_score || 0) >= 55) return '#ef4444';
        if (det.status === 'REVIEW' || (det.spoilage_score || 0) >= 35) return '#f59e0b';
        return '#22c55e';
    }
</script>

<div class="camera-stream-wrapper">
    <!-- Camera Viewport Card -->
    <div class="viewport-card">
        <div class="viewport-header">
            <div class="header-left">
                <span class="live-dot" class:offline={!hasCamera || streamError}></span>
                <strong>Live Inspection View</strong>
            </div>
            <span class="pill-badge">{hasCamera && !streamError ? 'MJPEG Stream (Zero Lag)' : 'Standby'}</span>
        </div>

        <div class="canvas-container">
            {#if hasCamera && !streamError}
                <!-- Native Browser MJPEG Stream with C++ Hardware-Accelerated Dynamic Bounding Boxes -->
                <img
                    src={activeStreamUrl}
                    alt="Live Video Stream"
                    class="feed-video"
                    onerror={() => (streamError = true)}
                />

                <!-- Dynamic Vector Overlay for Client-Side YOLO Detections (Only when box coordinates are present) -->
                {#if detections.some(d => d.box && d.box.length === 4)}
                    <svg class="detection-svg-overlay" viewBox="0 0 640 480" preserveAspectRatio="none">
                        {#each detections as det}
                            {#if det.box && det.box.length === 4}
                                {@const [bx, by, bw, bh] = det.box}
                                {@const color = getBoxColor(det)}
                                <!-- Dynamic Outer Bounding Box -->
                                <rect
                                    x={bx}
                                    y={by}
                                    width={bw}
                                    height={bh}
                                    rx="4"
                                    fill="none"
                                    stroke={color}
                                    stroke-width="2"
                                    opacity="0.95"
                                />

                                <!-- Tech Corner Accents -->
                                <path
                                    d={`M ${bx} ${by + 12} L ${bx} ${by} L ${bx + 12} ${by}
                                        M ${bx + bw - 12} ${by} L ${bx + bw} L ${bx + bw} ${by + 12}
                                        M ${bx} ${by + bh - 12} L ${bx} ${by + bh} L ${bx + 12} ${by + bh}
                                        M ${bx + bw - 12} ${by + bh} L ${bx + bw} ${by + bh} L ${bx + bw} ${by + bh - 12}`}
                                    fill="none"
                                    stroke={color}
                                    stroke-width="3.5"
                                    stroke-linecap="round"
                                />

                                <!-- Classification Label Pill -->
                                <g transform={`translate(${bx}, ${Math.max(0, by - 22)})`}>
                                    <rect
                                        x="0"
                                        y="0"
                                        width={det.food_name.length * 8 + 55}
                                        height="20"
                                        rx="4"
                                        fill={color}
                                    />
                                    <text
                                        x="6"
                                        y="14"
                                        fill="#0f172a"
                                        font-size="11"
                                        font-weight="bold"
                                        font-family="sans-serif"
                                    >
                                        {det.food_name.toUpperCase()} {(det.similarity_rate * 100).toFixed(0)}%
                                    </text>
                                </g>
                            {/if}
                        {/each}
                    </svg>
                {/if}
            {:else}
                <!-- Standby State -->
                <div class="standby-box">
                    <span class="standby-icon">📷</span>
                    <h4>No Camera Stream Connected</h4>
                    <p>Awaiting ESP32-S3 Camera Feed on /api/v1/camera/stream</p>
                    {#if streamError}
                        <button type="button" class="retry-btn" onclick={() => (streamError = false)}>
                            ↻ Reconnect Stream
                        </button>
                    {/if}
                </div>
            {/if}
        </div>

        <div class="viewport-footer">
            <span>📷 Device: {deviceId || 'ESP32S3_CAM'}</span>
            <span>⚡ Stream: MJPEG Multipart Push</span>
        </div>
    </div>

    <!-- Lovelace Entity Chips Grid (Like Home Assistant Switch Chips) -->
    <div class="entities-panel">
        <div class="panel-header">
            <div class="panel-title-group">
                <span class="entity-group-icon">🍱</span>
                <h3>Detected Items</h3>
            </div>
            <span class="active-count">{detections.length} Active</span>
        </div>

        <div class="entity-chips-grid">
            {#each standardFoodItems as item}
                {@const check = isDetected(item.id)}
                <div class="entity-item" class:on={check.found}>
                    <div class="entity-icon-circle">
                        <span>{item.icon}</span>
                    </div>
                    <div class="entity-info">
                        <span class="entity-name">{item.label}</span>
                        <span class="entity-state">{check.found ? `${(check.confidence * 100).toFixed(0)}% Match` : 'Off'}</span>
                    </div>
                </div>
            {/each}
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

    .pill-badge {
        font-size: 0.7rem;
        font-weight: 600;
        background: var(--bg-chip);
        color: var(--text-secondary);
        padding: 3px 10px;
        border-radius: var(--radius-pill);
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
        object-fit: cover;
        display: block;
    }

    .detection-svg-overlay {
        position: absolute;
        inset: 0;
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

    .entity-state {
        font-size: 0.65rem;
        color: var(--text-muted);
    }

    .entity-item.on .entity-state {
        color: var(--accent-blue);
        font-weight: 600;
    }
</style>
