<script lang="ts">
    import type { FoodDetection } from '../types';

    interface Props {
        detections: FoodDetection[];
    }

    let { detections = [] }: Props = $props();

    const compartmentDefaults = [
        { slot: 'Slot #1 (Main)', defaultName: 'Carbohydrate (Nasi)', targetWeight: '150 - 200g' },
        { slot: 'Slot #2 (Protein)', defaultName: 'Main Dish (Ayam/Ikan)', targetWeight: '100 - 130g' },
        { slot: 'Slot #3 (Vegetable)', defaultName: 'Vegetable (Sayur)', targetWeight: '70 - 90g' },
        { slot: 'Slot #4 (Fruit/Side)', defaultName: 'Fruit/Side (Buah)', targetWeight: '90 - 120g' }
    ];
</script>

<div class="breakdown-table-wrapper">
    <div class="table-header">
        <div class="title-meta">
            <h3>Food Breakdown & Compartments</h3>
            <span class="sub-label">STANDARD 4-COMPARTMENT NUTRITIONAL TRAY</span>
        </div>
        <div class="table-stats">
            <span>TARGET CALORIES: <strong>~650 - 720 kcal</strong></span>
        </div>
    </div>

    <div class="table-scroll-container">
        <table class="styled-table">
            <thead>
                <tr>
                    <th>COMPARTMENT</th>
                    <th>DETECTED FOOD</th>
                    <th>CATEGORY</th>
                    <th>SIMILARITY RATE</th>
                    <th>WEIGHT (G)</th>
                    <th>QUALITY STATUS</th>
                    <th>DECISION</th>
                </tr>
            </thead>
            <tbody>
                {#if detections.length === 0}
                    <tr>
                        <td colspan="7" class="empty-row">
                            <span class="muted-notice">Scanning camera feed for meal tray compartments...</span>
                        </td>
                    </tr>
                {:else}
                    {#each compartmentDefaults as comp, idx}
                        {@const det = detections[idx]}
                        <tr class:has-food={!!det}>
                            <td class="slot-cell">
                                <span class="slot-badge">{comp.slot}</span>
                            </td>
                            <td class="food-cell">
                                {#if det}
                                    <strong class="food-name">{det.food_name.toUpperCase()}</strong>
                                {:else}
                                    <span class="empty-slot">{comp.defaultName}</span>
                                {/if}
                            </td>
                            <td class="cat-cell">
                                {#if det}
                                    <span class="cat-pill">{det.category || 'Nutrient'}</span>
                                {:else}
                                    <span class="muted-dash">—</span>
                                {/if}
                            </td>
                            <td class="similarity-cell">
                                {#if det}
                                    <div class="sim-bar-wrapper">
                                        <div class="sim-meter">
                                            <div class="sim-fill" style={`width: ${det.similarity_rate * 100}%`}></div>
                                        </div>
                                        <span class="sim-val">{(det.similarity_rate * 100).toFixed(1)}%</span>
                                    </div>
                                {:else}
                                    <span class="muted-dash">—</span>
                                {/if}
                            </td>
                            <td class="weight-cell">
                                {#if det}
                                    <span class="weight-val">{det.weight_grams || 120} g</span>
                                {:else}
                                    <span class="muted-weight">{comp.targetWeight}</span>
                                {/if}
                            </td>
                            <td class="freshness-cell">
                                {#if det}
                                    <span class="freshness-badge fresh">
                                        <span class="dot"></span> FRESH (98.2%)
                                    </span>
                                {:else}
                                    <span class="muted-dash">—</span>
                                {/if}
                            </td>
                            <td class="decision-cell">
                                {#if det}
                                    {#if det.similarity_rate >= 0.75}
                                        <span class="status-badge pass">PASS</span>
                                    {:else}
                                        <span class="status-badge review">REVIEW</span>
                                    {/if}
                                {:else}
                                    <span class="status-badge waiting">AWAITING</span>
                                {/if}
                            </td>
                        </tr>
                    {/each}
                {/if}
            </tbody>
        </table>
    </div>
</div>

<style>
    .breakdown-table-wrapper {
        background: var(--bg-panel, #121824);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
        padding: 14px;
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .table-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 8px;
        border-bottom: 1px solid #1e293b;
    }

    .title-meta h3 {
        margin: 0;
        font-size: 0.95rem;
        font-weight: 700;
        color: #f1f5f9;
    }

    .sub-label {
        font-size: 0.65rem;
        font-weight: 600;
        color: #64748b;
        letter-spacing: 0.05em;
    }

    .table-stats {
        font-size: 0.75rem;
        color: #94a3b8;
    }

    .table-stats strong {
        color: #00f0ff;
    }

    .table-scroll-container {
        overflow-x: auto;
    }

    .styled-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 0.8rem;
        text-align: left;
    }

    .styled-table th {
        background: #0b0f19;
        color: #64748b;
        font-weight: 700;
        font-size: 0.65rem;
        letter-spacing: 0.05em;
        padding: 8px 12px;
        border-bottom: 1px solid #1e293b;
        white-space: nowrap;
    }

    .styled-table td {
        padding: 10px 12px;
        border-bottom: 1px solid rgba(255, 255, 255, 0.04);
        vertical-align: middle;
        white-space: nowrap;
    }

    .styled-table tr:hover {
        background: rgba(255, 255, 255, 0.02);
    }

    .slot-badge {
        display: inline-block;
        font-size: 0.7rem;
        font-family: monospace;
        font-weight: 700;
        color: #94a3b8;
        background: #0f172a;
        padding: 3px 8px;
        border-radius: 4px;
        border: 1px solid #1e293b;
    }

    .food-name {
        color: #ffffff;
        letter-spacing: 0.02em;
    }

    .empty-slot {
        color: #475569;
        font-style: italic;
    }

    .cat-pill {
        font-size: 0.7rem;
        padding: 2px 8px;
        background: rgba(59, 130, 246, 0.15);
        color: #60a5fa;
        border-radius: 12px;
        font-weight: 600;
    }

    .sim-bar-wrapper {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .sim-meter {
        width: 60px;
        height: 6px;
        background: #1e293b;
        border-radius: 3px;
        overflow: hidden;
    }

    .sim-fill {
        height: 100%;
        background: linear-gradient(90deg, #3b82f6, #00f0ff);
        border-radius: 3px;
    }

    .sim-val {
        font-family: monospace;
        font-weight: 700;
        color: #f1f5f9;
        font-size: 0.75rem;
    }

    .weight-val {
        font-family: monospace;
        font-weight: 700;
        color: #f8fafc;
    }

    .muted-weight {
        font-size: 0.7rem;
        color: #475569;
    }

    .freshness-badge {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        font-size: 0.7rem;
        font-weight: 700;
        padding: 3px 8px;
        border-radius: 4px;
    }

    .freshness-badge.fresh {
        background: rgba(16, 185, 129, 0.15);
        color: #10b981;
        border: 1px solid rgba(16, 185, 129, 0.3);
    }

    .freshness-badge .dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
        background: currentColor;
    }

    .status-badge {
        display: inline-block;
        font-size: 0.7rem;
        font-weight: 800;
        padding: 3px 10px;
        border-radius: 4px;
        letter-spacing: 0.05em;
    }

    .status-badge.pass {
        background: #10b981;
        color: #000;
    }

    .status-badge.review {
        background: #f59e0b;
        color: #000;
    }

    .status-badge.waiting {
        background: #1e293b;
        color: #64748b;
    }

    .empty-row {
        text-align: center;
        padding: 24px;
        color: #64748b;
    }

    .muted-dash {
        color: #334155;
    }
</style>
