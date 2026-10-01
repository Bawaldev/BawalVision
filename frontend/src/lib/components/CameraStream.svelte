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
        { id: 'nasi', label: 'Rice (Nasi)', icon: '🍚' },
        { id: 'ayam', label: 'Chicken (Ayam)', icon: '🍗' },
        { id: 'tofu', label: 'Tofu (Tahu)', icon: '🧊' },
        { id: 'pisang', label: 'Banana (Pisang)', icon: '🍌' },
        { id: 'jeruk', label: 'Orange (Jeruk)', icon: '🍊' },
        { id: 'bayam', label: 'Spinach (Bayam)', icon: '🥬' },
        { id: 'kangkung', label: 'Water Spinach', icon: '🌱' },
        { id: 'others', label: 'Side Dish', icon: '🍱' }
    ];

    let streamError = $state(false);

    let activeStreamUrl = $derived(
        streamUrl || (deviceId ? `/api/v1/camera/stream/${deviceId}` : '/api/v1/camera/stream')
    );

    function isDetected(id: string): { found: boolean; confidence: number } {
        if (!hasCamera || detections.length === 0) {
            return { found: false, confidence: 0 };
        }
        if (id === 'others') {
            const other = detections.find(d => !standardFoodItems.slice(0, 7).some(s => s.id === d.food_name.toLowerCase()));
            return { found: !!other, confidence: other ? other.similarity_rate : 0 };
        }
        const item = detections.find(d => d.food_name.toLowerCase() === id.toLowerCase());
        return { found: !!item, confidence: item ? item.similarity_rate : 0 };
    }

    const compartments = [
        { id: 'carb', name: 'SLOT 1: CARBOHYDRATE', x: 45, y: 42, w: 220, h: 396, color: '#38bdf8' },
        { id: 'protein', name: 'SLOT 2: PROTEIN', x: 275, y: 42, w: 320, h: 190, color: '#f43f5e' },
        { id: 'veggie', name: 'SLOT 3: VEGETABLE', x: 275, y: 242, w: 155, h: 196, color: '#22c55e' },
        { id: 'fruit', name: 'SLOT 4: FRUIT / SIDE', x: 440, y: 242, w: 155, h: 196, color: '#c084fc' }
    ];

    function getMatchedDetection(compId: string, slotIdx: number): FoodDetection | null {
        if (detections.length === 0) return null;
        let det = detections.find(d => {
            const name = d.food_name.toLowerCase();
            if (compId === 'carb' && (name.includes('nasi') || name.includes('rice') || d.category === 'Carbohydrate')) return true;
            if (compId === 'protein' && (name.includes('ayam') || name.includes('chicken') || name.includes('tofu') || d.category === 'Protein')) return true;
            if (compId === 'veggie' && (name.includes('kangkung') || name.includes('bayam') || name.includes('spinach') || d.category === 'Vegetable')) return true;
            if (compId === 'fruit' && (name.includes('pisang') || name.includes('jeruk') || name.includes('banana') || name.includes('orange') || d.category === 'Fruit')) return true;
            return false;
        });
        if (!det && detections[slotIdx]) {
            return detections[slotIdx];
        }
        return det || null;
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
                <!-- Native Browser MJPEG Stream for Zero Delay Hardware Rendering -->
                <img
                    src={activeStreamUrl}
                    alt="Live Video Stream"
                    class="feed-video"
                    onerror={() => (streamError = true)}
                />

                <!-- Crisp Vector Overlay for Food Detection Compartments -->
                <svg class="detection-svg-overlay" viewBox="0 0 640 480" preserveAspectRatio="none">
                    {#each compartments as comp, idx}
                        {@const det = getMatchedDetection(comp.id, idx)}
                        {#if det}
                            <!-- Outer Bounding Box -->
                            <rect
                                x={comp.x}
                                y={comp.y}
                                width={comp.w}
                                height={comp.h}
                                rx="8"
                                fill="none"
                                stroke={comp.color}
                                stroke-width="2"
                                opacity="0.9"
                            />

                            <!-- Tech Corner Accents -->
                            <path
                                d={`M ${comp.x} ${comp.y + 16} L ${comp.x} ${comp.y} L ${comp.x + 16} ${comp.y}
                                    M ${comp.x + comp.w - 16} ${comp.y} L ${comp.x + comp.w} ${comp.y} L ${comp.x + comp.w} ${comp.y + 16}
                                    M ${comp.x} ${comp.y + comp.h - 16} L ${comp.x} ${comp.y + comp.h} L ${comp.x + 16} ${comp.y + comp.h}
                                    M ${comp.x + comp.w - 16} ${comp.y + comp.h} L ${comp.x + comp.w} ${comp.y + comp.h} L ${comp.x + comp.w} ${comp.y + comp.h - 16}`}
                                fill="none"
                                stroke={comp.color}
                                stroke-width="3.5"
                                stroke-linecap="round"
                            />

                            <!-- Classification Label Pill -->
                            <g transform={`translate(${comp.x}, ${comp.y - 20})`}>
                                <rect
                                    x="0"
                                    y="0"
                                    width={det.food_name.length * 8 + 60}
                                    height="20"
                                    rx="4"
                                    fill={comp.color}
                                />
                                <text
                                    x="8"
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
