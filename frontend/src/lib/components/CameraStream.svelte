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
        { id: 'nasi', label: 'Rice (Nasi)' },
        { id: 'ayam', label: 'Chicken (Ayam)' },
        { id: 'tofu', label: 'Tofu (Tahu)' },
        { id: 'pisang', label: 'Banana (Pisang)' },
        { id: 'jeruk', label: 'Orange (Jeruk)' },
        { id: 'bayam', label: 'Spinach (Bayam)' },
        { id: 'kangkung', label: 'Water Spinach' },
        { id: 'others', label: 'Others' }
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
        // Track reactive dependencies
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

        // Dark Industrial Canvas Background
        ctx.fillStyle = '#05070e';
        ctx.fillRect(0, 0, w, h);

        if (!hasCamera) {
            // ==========================================
            // NO CAMERA STREAM / OFFLINE STANDBY STATE
            // ==========================================
            ctx.save();
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.03)';
            ctx.lineWidth = 1;
            for (let x = 40; x < w; x += 40) {
                ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
            }
            for (let y = 40; y < h; y += 40) {
                ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
            }

            const boxW = 380;
            const boxH = 130;
            const boxX = (w - boxW) / 2;
            const boxY = (h - boxH) / 2;

            ctx.fillStyle = 'rgba(15, 23, 42, 0.9)';
            ctx.strokeStyle = '#334155';
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            ctx.roundRect(boxX, boxY, boxW, boxH, 8);
            ctx.fill();
            ctx.stroke();

            ctx.fillStyle = '#f59e0b';
            ctx.font = 'bold 22px sans-serif';
            ctx.textAlign = 'center';
            ctx.fillText('⚠', w / 2, boxY + 36);

            ctx.fillStyle = '#f8fafc';
            ctx.font = 'bold 13px monospace';
            ctx.fillText('NO CAMERA STREAM DETECTED', w / 2, boxY + 62);

            ctx.fillStyle = '#64748b';
            ctx.font = '11px monospace';
            ctx.fillText('Awaiting ESP32-S3 / USB Camera Ingress Feed', w / 2, boxY + 84);
            ctx.fillText('Status: Standby (0 FPS) | Vision Service Idle', w / 2, boxY + 104);

            ctx.textAlign = 'left';
            ctx.restore();
            return;
        }

        // 1. Draw Live Camera Video Frame
        if (cameraImg && cameraImg.complete && cameraImg.naturalWidth > 0 && !imageLoadFailed) {
            ctx.drawImage(cameraImg, 0, 0, w, h);
        } else {
            // Camera connected but waiting for first frame
            ctx.fillStyle = '#060911';
            ctx.fillRect(0, 0, w, h);
        }

        // 2. Define Standard Tray Compartment Coordinates for Bounding Boxes
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
                color: '#00f0ff'
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
                color: '#10b981'
            },
            {
                id: 'fruit',
                name: 'SLOT 4: FRUIT / SIDE',
                x: midX + (compPad * 0.5) + (((trayX + trayW - midX) - (compPad * 2.5)) / 2) + compPad,
                y: midY + (compPad * 0.5),
                w: ((trayX + trayW - midX) - (compPad * 2.5)) / 2,
                h: (trayY + trayH - midY) - (compPad * 1.5),
                color: '#a855f7'
            }
        ];

        // 3. Draw AI Detection Bounding Boxes (Transparent - ONLY when foods are detected!)
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
                // Bounding Box Outline (Transparent fill)
                ctx.strokeStyle = comp.color;
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.roundRect(bx, by, bw, bh, 6);
                ctx.stroke();

                // Corner Brackets
                ctx.lineWidth = 3.5;
                ctx.beginPath();
                ctx.moveTo(bx, by + bl); ctx.lineTo(bx, by); ctx.lineTo(bx + bl, by);
                ctx.moveTo(bx + bw - bl, by); ctx.lineTo(bx + bw, by); ctx.lineTo(bx + bw, by + bl);
                ctx.moveTo(bx, by + bh - bl); ctx.lineTo(bx, by + bh); ctx.lineTo(bx + bl, by + bh);
                ctx.moveTo(bx + bw - bl, by + bh); ctx.lineTo(bx + bw, by + bh); ctx.lineTo(bx + bw, by + bh - bl);
                ctx.stroke();

                // Detection Badge
                const labelText = `${det.food_name.toUpperCase()} ${(det.similarity_rate * 100).toFixed(0)}%`;
                ctx.font = 'bold 11px monospace';
                const metrics = ctx.measureText(labelText);
                const badgeW = metrics.width + 14;
                const badgeH = 20;

                ctx.fillStyle = comp.color;
                ctx.beginPath();
                ctx.roundRect(bx, by - badgeH + 2, badgeW, badgeH, [4, 4, 0, 0]);
                ctx.fill();

                ctx.fillStyle = '#05070d';
                ctx.fillText(labelText, bx + 7, by - 6);
                ctx.restore();
            });
        }

        // 4. Subtle Industrial HUD Overlay (Transparent)
        ctx.save();
        ctx.fillStyle = 'rgba(0, 240, 255, 0.9)';
        ctx.font = '600 10px monospace';
        ctx.fillText(`CAM_FEED: ${deviceId || 'LIVE'} | STREAM: OV5640_HQ`, 16, 18);
        ctx.fillText(`C++ ENGINE: MOBILENETV3_ONNX`, w - 190, 18);
        ctx.restore();
    }
</script>

<div class="camera-stream-wrapper">
    <!-- Main Camera Viewport -->
    <div class="viewport-box">
        <div class="viewport-header">
            <div class="live-indicator">
                <span class="pulse-dot" class:offline={!hasCamera}></span>
                <strong>OPTICAL INSPECTION POD</strong>
            </div>
            <div class="resolution-tag">{hasCamera ? '640 × 480 @ 30FPS' : 'OFFLINE - NO SIGNAL'}</div>
        </div>

        <div class="canvas-container">
            <canvas
                bind:this={canvasEl}
                width="640"
                height="480"
                class="feed-canvas"
            ></canvas>

            <!-- Crosshair Center Overlay -->
            <div class="hud-reticle"></div>
        </div>

        <div class="viewport-footer">
            <span>PERSPECTIVE: {hasCamera ? 'DEWARPED (HOMOGRAPHY CALIBRATED)' : 'STANDBY'}</span>
            <span>AI ENGINE: MOBILENETV3_ONNX</span>
        </div>
    </div>

    <!-- Detected Objects Checklist (Wireframe: Detected Object Panel) -->
    <div class="detected-objects-panel">
        <div class="panel-header">
            <h3>Detected Object</h3>
            <span class="count-badge">{detections.length} Active</span>
        </div>

        <div class="food-checklist">
            {#each standardFoodItems as item}
                {@const check = isDetected(item.id)}
                <div class="food-check-row" class:active={check.found}>
                    <div class="check-box" class:checked={check.found}>
                        {#if check.found}
                            ✓
                        {/if}
                    </div>
                    <span class="food-label">{item.label}</span>
                    {#if check.found}
                        <span class="conf-badge">{(check.confidence * 100).toFixed(0)}%</span>
                    {/if}
                </div>
            {/each}
        </div>
    </div>
</div>

<style>
    .camera-stream-wrapper {
        display: grid;
        grid-template-columns: 1fr 220px;
        gap: 16px;
        background: var(--bg-panel, #121824);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
        padding: 14px;
    }

    .viewport-box {
        display: flex;
        flex-direction: column;
        background: #080c14;
        border: 1px solid #1e293b;
        border-radius: 6px;
        overflow: hidden;
    }

    .viewport-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 8px 12px;
        background: rgba(0, 0, 0, 0.4);
        border-bottom: 1px solid #1e293b;
    }

    .live-indicator {
        display: flex;
        align-items: center;
        gap: 8px;
        font-size: 0.75rem;
        color: #f8fafc;
        letter-spacing: 0.05em;
    }

    .pulse-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #10b981;
        box-shadow: 0 0 8px #10b981;
        animation: blink 1.5s infinite;
    }

    .pulse-dot.offline {
        background: #64748b;
        box-shadow: none;
        animation: none;
    }

    @keyframes blink {
        0%, 100% { opacity: 1; }
        50% { opacity: 0.3; }
    }

    .resolution-tag {
        font-size: 0.65rem;
        font-family: monospace;
        color: #64748b;
        background: #0f172a;
        padding: 2px 6px;
        border-radius: 4px;
        border: 1px solid #1e293b;
    }

    .canvas-container {
        position: relative;
        width: 100%;
        background: #000;
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
        padding: 6px 12px;
        font-size: 0.65rem;
        color: #475569;
        font-family: monospace;
        background: rgba(0, 0, 0, 0.3);
        border-top: 1px solid #1e293b;
    }

    .detected-objects-panel {
        display: flex;
        flex-direction: column;
        background: #0b0f19;
        border: 1px solid #1e293b;
        border-radius: 6px;
        padding: 12px;
    }

    .panel-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 10px;
        border-bottom: 1px solid #1e293b;
        margin-bottom: 10px;
    }

    .panel-header h3 {
        margin: 0;
        font-size: 0.9rem;
        font-weight: 700;
        color: #f1f5f9;
        letter-spacing: -0.01em;
    }

    .count-badge {
        font-size: 0.65rem;
        font-weight: 700;
        padding: 2px 6px;
        background: rgba(0, 240, 255, 0.15);
        color: #00f0ff;
        border-radius: 4px;
        border: 1px solid rgba(0, 240, 255, 0.3);
    }

    .food-checklist {
        display: flex;
        flex-direction: column;
        gap: 6px;
        flex: 1;
    }

    .food-check-row {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 7px 10px;
        background: rgba(255, 255, 255, 0.02);
        border: 1px solid rgba(255, 255, 255, 0.04);
        border-radius: 6px;
        transition: all 0.2s ease;
    }

    .food-check-row.active {
        background: rgba(0, 240, 255, 0.08);
        border-color: rgba(0, 240, 255, 0.35);
    }

    .check-box {
        width: 18px;
        height: 18px;
        border-radius: 4px;
        border: 1.5px solid #334155;
        display: flex;
        align-items: center;
        justify-content: center;
        font-size: 0.75rem;
        font-weight: 900;
        color: #000;
        background: transparent;
        flex-shrink: 0;
    }

    .check-box.checked {
        background: #00f0ff;
        border-color: #00f0ff;
    }

    .food-label {
        font-size: 0.8rem;
        font-weight: 600;
        color: #94a3b8;
        flex: 1;
    }

    .food-check-row.active .food-label {
        color: #ffffff;
    }

    .conf-badge {
        font-size: 0.7rem;
        font-family: monospace;
        font-weight: 700;
        color: #10b981;
    }
</style>
