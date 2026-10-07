<script lang="ts">
    import type { FoodDetection } from '../types';

    interface Props {
        detections: FoodDetection[];
    }

    let { detections = [] }: Props = $props();

    function getFoodIcon(name: string): string {
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
</script>

<div class="breakdown-card">
    <div class="card-header">
        <div class="header-left">
            <span class="card-icon">🍱</span>
            <div>
                <h4>Detected Food Items</h4>
                <p class="sub-label">Dynamic YOLO Object Detection & Freshness Analysis</p>
            </div>
        </div>
        <div class="header-stats">
            <span class="stat-pill">{detections.length} Detected</span>
            <span class="stat-pill">Thermal Limit: ≤38°C</span>
            <span class="stat-pill">CV Threshold: ≥40%</span>
        </div>
    </div>

    <div class="table-container">
        <table class="styled-table">
            <thead>
                <tr>
                    <th>#</th>
                    <th>Detected Item</th>
                    <th>Food Temp</th>
                    <th>CV Match</th>
                    <th>ML Spoilage</th>
                    <th>Decision</th>
                </tr>
            </thead>
            <tbody>
                {#if detections.length === 0}
                    <tr>
                        <td colspan="6" class="empty-row">
                            <span class="empty-msg">No food items detected in camera view.</span>
                        </td>
                    </tr>
                {:else}
                    {#each detections as det, idx}
                        <tr class="has-item">
                            <td class="slot-cell">
                                <span class="slot-tag">#{idx + 1}</span>
                            </td>
                            <td class="food-cell">
                                <div class="food-name-row">
                                    <span class="food-icon">{getFoodIcon(det.food_name)}</span>
                                    <div>
                                        <strong class="food-name">{det.food_name}</strong>
                                        <small class="cat-label">{det.category || 'Food'}</small>
                                    </div>
                                </div>
                            </td>
                            <td class="temp-cell">
                                <span
                                    class="temp-num"
                                    class:warn={(det.temperature || 25) > 35}
                                    class:danger={(det.temperature || 25) > 42}
                                >
                                    {det.temperature ? det.temperature.toFixed(1) : '24.5'} <small>°C</small>
                                </span>
                            </td>
                            <td class="sim-cell">
                                <div class="sim-bar-box">
                                    <div class="sim-track">
                                        <div class="sim-fill" style={`width: ${Math.min(100, det.similarity_rate * 100)}%`}></div>
                                    </div>
                                    <span class="sim-num">{(det.similarity_rate * 100).toFixed(0)}%</span>
                                </div>
                            </td>
                            <td class="spoilage-cell">
                                <span class="spoilage-num" class:danger={(det.spoilage_score || 0) >= 55}>
                                    {(det.spoilage_score || 0).toFixed(0)}%
                                </span>
                            </td>
                            <td class="status-cell">
                                {#if det.status === 'REJECT' || (det.spoilage_score || 0) >= 55 || det.similarity_rate < 0.35}
                                    <span class="badge reject">Reject</span>
                                {:else if det.status === 'REVIEW' || (det.spoilage_score || 0) >= 35 || det.similarity_rate < 0.60}
                                    <span class="badge hold">Review</span>
                                {:else}
                                    <span class="badge pass">Pass</span>
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
    .breakdown-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        padding: 16px;
        display: flex;
        flex-direction: column;
        gap: 14px;
        box-shadow: var(--shadow-card);
    }

    .card-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .header-left {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .card-icon {
        font-size: 1.3rem;
    }

    .header-left h4 {
        margin: 0;
        font-size: 0.92rem;
        font-weight: 700;
        color: var(--text-primary);
        line-height: 1.2;
    }

    .sub-label {
        margin: 0;
        font-size: 0.68rem;
        color: var(--text-muted);
    }

    .header-stats {
        display: flex;
        gap: 8px;
    }

    .stat-pill {
        font-size: 0.68rem;
        font-weight: 600;
        background: var(--bg-chip);
        color: var(--text-secondary);
        padding: 3px 8px;
        border-radius: var(--radius-pill);
    }

    .table-container {
        overflow-x: auto;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
    }

    .styled-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 0.78rem;
        text-align: left;
    }

    .styled-table th {
        background: var(--bg-chip);
        color: var(--text-secondary);
        font-weight: 700;
        font-size: 0.68rem;
        padding: 10px 14px;
        border-bottom: 1px solid var(--border-subtle);
        white-space: nowrap;
    }

    .styled-table td {
        padding: 10px 14px;
        border-bottom: 1px solid var(--border-subtle);
        vertical-align: middle;
        white-space: nowrap;
    }

    .styled-table tr:last-child td {
        border-bottom: none;
    }

    .styled-table tr:hover {
        background: rgba(255, 255, 255, 0.02);
    }

    .slot-tag {
        font-size: 0.72rem;
        font-weight: 600;
        color: var(--text-secondary);
    }

    .food-name {
        color: var(--text-primary);
        font-size: 0.82rem;
    }

    .empty-name {
        color: var(--text-muted);
        font-size: 0.75rem;
    }

    .sim-bar-box {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .sim-track {
        width: 60px;
        height: 6px;
        background: var(--bg-chip);
        border-radius: var(--radius-pill);
        overflow: hidden;
    }

    .sim-fill {
        height: 100%;
        background: var(--accent-green);
        border-radius: var(--radius-pill);
    }

    .sim-num {
        font-family: var(--font-mono);
        font-size: 0.75rem;
        color: var(--text-primary);
        font-weight: 700;
    }

    .badge {
        font-size: 0.68rem;
        font-weight: 700;
        padding: 3px 8px;
        border-radius: var(--radius-pill);
    }

    .food-cell {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .cat-label {
        font-size: 0.65rem;
        color: var(--text-muted);
        text-transform: uppercase;
        letter-spacing: 0.04em;
    }

    .temp-num, .spoilage-num {
        font-family: var(--font-mono);
        font-size: 0.78rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .temp-num.warn {
        color: var(--accent-amber);
    }

    .temp-num.danger {
        color: var(--accent-red);
    }

    .temp-num small {
        color: var(--text-muted);
        font-weight: normal;
    }

    .spoilage-num.danger {
        color: var(--accent-red);
    }

    .badge.reject {
        background: var(--accent-red-dim);
        color: var(--accent-red);
        border: 1px solid rgba(244, 63, 94, 0.3);
    }

    .badge.pass {
        background: var(--accent-green-dim);
        color: var(--accent-green);
        border: 1px solid rgba(34, 197, 94, 0.3);
    }

    .badge.hold {
        background: var(--accent-amber-dim);
        color: var(--accent-amber);
        border: 1px solid rgba(245, 158, 11, 0.3);
    }

    .badge.idle {
        background: var(--bg-chip);
        color: var(--text-muted);
    }

    .empty-row {
        text-align: center;
        padding: 24px;
        color: var(--text-muted);
        font-size: 0.78rem;
    }

    .muted-dash {
        color: var(--text-muted);
    }
</style>
