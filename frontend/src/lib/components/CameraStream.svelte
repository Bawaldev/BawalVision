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
    let streamEpoch = $state(Date.now());

    let activeStreamUrl = $derived(
        streamUrl || `/api/v1/camera/stream?t=${streamEpoch}`
    );

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

    // Auto-recovery watchdog: auto-reconnect stream if stalled
    $effect(() => {
        if (!hasCamera) return;
        const interval = setInterval(() => {
            if (streamError) {
                streamEpoch = Date.now();
                streamError = false;
            }
        }, 3500);
        return () => clearInterval(interval);
    });
</script>

<div class="camera-stream-wrapper">
    <!-- Camera Viewport Card -->
    <div class="viewport-card">
        <div class="viewport-header">
            <div class="header-left">
                <span class="live-dot" class:offline={!hasCamera || streamError}></span>
                <strong>Live Inspection View</strong>
            </div>
            <span class="pill-badge">{hasCamera && !streamError ? 'MJPEG Stream (Active HUD)' : 'Standby'}</span>
        </div>

        <div class="canvas-container">
            {#if hasCamera && !streamError}
                <!-- Native Browser MJPEG Stream with C++ Hardware-Accelerated Dynamic Bounding Boxes -->
                <img
                    src={activeStreamUrl}
                    alt="Live Video Stream"
                    class="feed-video"
                    onload={() => (streamError = false)}
                    onerror={() => (streamError = true)}
                />
            {:else}
                <!-- Standby State -->
                <div class="standby-box">
                    <span class="standby-icon">📷</span>
                    <h4>No Camera Stream Connected</h4>
                    <p>Awaiting ESP32-S3 Camera Feed on /api/v1/camera/stream</p>
                    {#if streamError}
                        <button type="button" class="retry-btn" onclick={() => { streamError = false; streamEpoch = Date.now(); }}>
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

    .entity-state-row {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-top: 1px;
    }

    .entity-state {
        font-size: 0.65rem;
        color: var(--text-muted);
    }

    .entity-item.on .entity-state {
        color: var(--text-secondary);
        font-weight: 600;
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
</style>
