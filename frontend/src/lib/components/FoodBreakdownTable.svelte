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
        if (lower.includes('tempeh') || lower.includes('tempe')) return '🥢';
        if (lower.includes('tofu') || lower.includes('tahu')) return '🧈';
        if (lower.includes('banana') || lower.includes('pisang')) return '🍌';
        if (lower.includes('orange') || lower.includes('jeruk')) return '🍊';
        if (lower.includes('water_spinach') || lower.includes('kangkung')) return '🥬';
        if (lower.includes('spinach') || lower.includes('bayam')) return '🥗';
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
            <span class="stat-pill">CV Threshold: &ge;40%</span>
        </div>
    </div>

    <div class="table-container">
        <table class="styled-table">
            <thead>
                <tr>
                    <th>#</th>
                    <th>Detected Item</th>
                    <th>CV Match</th>
                    <th>ML Spoilage</th>
                    <th>Decision</th>
                </tr>
            </thead>
            <tbody>
                {#if detections.length === 0}
                    <tr>
                        <td colspan="5" class="empty-row">
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
                                    <div class="food-info-text">
                                        <strong class="food-name">{det.food_name}</strong>
                                        <small class="cat-label">{det.category || 'Food'}</small>
                                    </div>
                                </div>
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
        background: #ffffff;
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
        flex-wrap: wrap;
        gap: 10px;
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
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
    }

    .stat-pill {
        font-size: 0.68rem;
        font-weight: 600;
        background: #f1f5f9;
        color: #475569;
        padding: 3px 8px;
        border-radius: var(--radius-pill);
        border: 1px solid #e2e8f0;
    }

    .table-container {
        overflow-x: auto;
        background: #ffffff;
        border: 1px solid #e2e8f0;
        border-radius: var(--radius-md);
    }

    .styled-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 0.78rem;
        text-align: left;
    }

    .styled-table th {
        background: #f8fafc;
        color: #475569;
        font-weight: 700;
        font-size: 0.68rem;
        padding: 10px 14px;
        border-bottom: 1px solid #e2e8f0;
        white-space: nowrap;
        text-transform: uppercase;
        letter-spacing: 0.03em;
    }

    .styled-table td {
        padding: 10px 14px;
        border-bottom: 1px solid #e2e8f0;
        vertical-align: middle;
        white-space: nowrap;
    }

    .styled-table tr:last-child td {
        border-bottom: none;
    }

    .styled-table tr.has-item:hover {
        background: #f8fafc;
    }

    .slot-tag {
        font-size: 0.72rem;
        font-weight: 700;
        color: #64748b;
        font-family: var(--font-mono);
    }

    .food-name-row {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .food-icon {
        font-size: 1.1rem;
    }

    .food-info-text {
        display: flex;
        flex-direction: column;
    }

    .food-name {
        color: #1e293b;
        font-size: 0.82rem;
        font-weight: 700;
    }

    .cat-label {
        font-size: 0.62rem;
        color: #64748b;
        text-transform: uppercase;
        letter-spacing: 0.04em;
        font-weight: 600;
    }

    .sim-bar-box {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .sim-track {
        width: 60px;
        height: 6px;
        background: #e2e8f0;
        border-radius: var(--radius-pill);
        overflow: hidden;
    }

    .sim-fill {
        height: 100%;
        background: #16a34a;
        border-radius: var(--radius-pill);
    }

    .sim-num {
        font-family: var(--font-mono);
        font-size: 0.75rem;
        color: #1e293b;
        font-weight: 700;
    }

    .spoilage-num {
        font-family: var(--font-mono);
        font-size: 0.78rem;
        font-weight: 700;
        color: #1e293b;
    }

    .spoilage-num.danger {
        color: #dc2626;
    }

    .badge {
        font-size: 0.68rem;
        font-weight: 700;
        padding: 3px 8px;
        border-radius: var(--radius-pill);
    }

    .badge.reject {
        background: #fee2e2;
        color: #dc2626;
        border: 1px solid #fecaca;
    }

    .badge.pass {
        background: #dcfce7;
        color: #16a34a;
        border: 1px solid #bbf7d0;
    }

    .badge.hold {
        background: #fef3c7;
        color: #d97706;
        border: 1px solid #fde68a;
    }

    .empty-row {
        text-align: center;
        padding: 24px;
        color: #64748b;
        font-size: 0.78rem;
    }
</style>
