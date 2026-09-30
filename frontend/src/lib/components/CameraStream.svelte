<script lang="ts">
    import { onMount } from 'svelte';
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

    let canvasEl = $state<HTMLCanvasElement | null>(null);
    let cameraImg: HTMLImageElement | null = null;
    let imageLoadFailed = $state(false);

    onMount(() => {
        cameraImg = new Image();
        cameraImg.crossOrigin = 'anonymous';

        cameraImg.onload = () => {
            imageLoadFailed = false;
            renderCanvas();
        };

        cameraImg.onerror = () => {
            imageLoadFailed = true;
        };

        const frameTimer = setInterval(() => {
            if (hasCamera && cameraImg) {
                const url = streamUrl || (deviceId ? `/api/v1/camera/${deviceId}/raw` : '/api/v1/camera/latest/raw');
                cameraImg.src = `${url}?t=${Date.now()}`;
            }
        }, 500);

        return () => {
            clearInterval(frameTimer);
        };
    });

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

    $effect(() => {
        const _d = detections;
        const _c = hasCamera;
        const _id = deviceId;
        renderCanvas();
    });

    function renderCanvas() {
        if (!canvasEl) return;
        const ctx = canvasEl.getContext('2d');
        if (!ctx) return;

        const w = canvasEl.width;
        const h = canvasEl.height;

        ctx.fillStyle = '#14171d';
        ctx.fillRect(0, 0, w, h);

        if (!hasCamera) {
            // Standby state
            ctx.save();
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.04)';
            ctx.lineWidth = 1;
            for (let x = 40; x < w; x += 40) {
                ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
            }
            for (let y = 40; y < h; y += 40) {
                ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
            }

            const boxW = 340;
            const boxH = 120;
            const boxX = (w - boxW) / 2;
            const boxY = (h - boxH) / 2;

            ctx.fillStyle = '#202530';
            ctx.strokeStyle = '#2d3444';
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            ctx.roundRect(boxX, boxY, boxW, boxH, 12);
            ctx.fill();
            ctx.stroke();

            ctx.fillStyle = '#f59e0b';
            ctx.font = 'bold 24px sans-serif';
            ctx.textAlign = 'center';
            ctx.fillText('📷', w / 2, boxY + 36);

            ctx.fillStyle = '#f8fafc';
            ctx.font = 'bold 13px sans-serif';
            ctx.fillText('No Camera Stream Connected', w / 2, boxY + 66);

            ctx.fillStyle = '#94a3b8';
            ctx.font = '11px sans-serif';
            ctx.fillText('Awaiting ESP32-S3 Camera Feed on /api/v1/camera', w / 2, boxY + 88);

            ctx.textAlign = 'left';
            ctx.restore();
            return;
        }

        // Draw Camera Frame
        if (cameraImg && cameraImg.complete && cameraImg.naturalWidth > 0 && !imageLoadFailed) {
            ctx.drawImage(cameraImg, 0, 0, w, h);
        } else {
            ctx.fillStyle = '#181c24';
            ctx.fillRect(0, 0, w, h);
        }

        // Draw Compartment Overlays
        const trayX = 35;
        const trayY = 32;
        const trayW = w - 70;
        const trayH = h - 64;
        const compPad = 10;
        const midX = trayX + (trayW * 0.46);
        const midY = trayY + (trayH * 0.50);

        const compLayouts = [
            {
                id: 'carb',
                name: 'SLOT 1: CARBOHYDRATE',
                x: trayX + compPad,
                y: trayY + compPad,
                w: (midX - trayX) - (compPad * 1.5),
                h: trayH - (compPad * 2),
                color: '#38bdf8'
            },
            {
                id: 'protein',
                name: 'SLOT 2: PROTEIN',
                x: midX + (compPad * 0.5),
                y: trayY + compPad,
                w: (trayX + trayW - midX) - (compPad * 1.5),
                h: (midY - trayY) - (compPad * 1.5),
                color: '#f43f5e'
            },
            {
                id: 'veggie',
                name: 'SLOT 3: VEGETABLE',
                x: midX + (compPad * 0.5),
                y: midY + (compPad * 0.5),
                w: ((trayX + trayW - midX) - (compPad * 2.5)) / 2,
                h: (trayY + trayH - midY) - (compPad * 1.5),
                color: '#22c55e'
            },
            {
                id: 'fruit',
                name: 'SLOT 4: FRUIT / SIDE',
                x: midX + (compPad * 0.5) + (((trayX + trayW - midX) - (compPad * 2.5)) / 2) + compPad,
                y: midY + (compPad * 0.5),
                w: ((trayX + trayW - midX) - (compPad * 2.5)) / 2,
                h: (trayY + trayH - midY) - (compPad * 1.5),
                color: '#c084fc'
            }
        ];

        if (detections.length > 0) {
            compLayouts.forEach((comp, slotIdx) => {
                let det = detections.find(d => {
                    const name = d.food_name.toLowerCase();
                    if (comp.id === 'carb' && (name.includes('nasi') || name.includes('rice') || d.category === 'Carbohydrate')) return true;
                    if (comp.id === 'protein' && (name.includes('ayam') || name.includes('chicken') || name.includes('tofu') || d.category === 'Protein')) return true;
                    if (comp.id === 'veggie' && (name.includes('kangkung') || name.includes('bayam') || name.includes('spinach') || d.category === 'Vegetable')) return true;
                    if (comp.id === 'fruit' && (name.includes('pisang') || name.includes('jeruk') || name.includes('banana') || name.includes('orange') || d.category === 'Fruit')) return true;
                    return false;
                });

                if (!det && detections[slotIdx]) {
                    det = detections[slotIdx];
                }

                if (!det) return;

                const bx = comp.x + 4;
                const by = comp.y + 4;
                const bw = comp.w - 8;
                const bh = comp.h - 8;
                const bl = 12;

                ctx.save();
                ctx.strokeStyle = comp.color;
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.roundRect(bx, by, bw, bh, 8);
                ctx.stroke();

                ctx.lineWidth = 3.5;
                ctx.beginPath();
                ctx.moveTo(bx, by + bl); ctx.lineTo(bx, by); ctx.lineTo(bx + bl, by);
                ctx.moveTo(bx + bw - bl, by); ctx.lineTo(bx + bw, by); ctx.lineTo(bx + bw, by + bl);
                ctx.moveTo(bx, by + bh - bl); ctx.lineTo(bx, by + bh); ctx.lineTo(bx + bl, by + bh);
                ctx.moveTo(bx + bw - bl, by + bh); ctx.lineTo(bx + bw, by + bh); ctx.lineTo(bx + bw, by + bh - bl);
                ctx.stroke();

                const labelText = `${det.food_name} ${(det.similarity_rate * 100).toFixed(0)}%`;
                ctx.font = 'bold 11px sans-serif';
                const metrics = ctx.measureText(labelText);
                const badgeW = metrics.width + 16;
                const badgeH = 22;

                ctx.fillStyle = comp.color;
                ctx.beginPath();
                ctx.roundRect(bx, by - badgeH + 2, badgeW, badgeH, [6, 6, 0, 0]);
                ctx.fill();

                ctx.fillStyle = '#0f172a';
                ctx.fillText(labelText, bx + 8, by - 6);
                ctx.restore();
            });
        }
    }
</script>

<div class="camera-stream-wrapper">
    <!-- Camera Viewport Card -->
    <div class="viewport-card">
        <div class="viewport-header">
            <div class="header-left">
                <span class="live-dot" class:offline={!hasCamera}></span>
                <strong>Live Inspection View</strong>
            </div>
            <span class="pill-badge">{hasCamera ? '640 × 480 @ 30 FPS' : 'Standby'}</span>
        </div>

        <div class="canvas-container">
            <canvas
                bind:this={canvasEl}
                width="640"
                height="480"
                class="feed-canvas"
            ></canvas>
        </div>

        <div class="viewport-footer">
            <span>📷 Device: {deviceId || 'ESP32S3_CAM'}</span>
            <span>⚡ Engine: C++ SIMD</span>
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
    }

    .feed-canvas {
        width: 100%;
        height: 100%;
        object-fit: contain;
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
