<script lang="ts">
    import type { FoodDetection } from '../types';

    interface Props {
        detections: FoodDetection[];
    }

    let { detections = [] }: Props = $props();

    type TableFilterMode = 'ALL' | 'PASS' | 'REVIEW' | 'REJECT';
    let filterMode = $state<TableFilterMode>('ALL');

    function getItemDecision(det: FoodDetection | null | undefined): 'PASS' | 'REVIEW' | 'REJECT' {
        if (!det) return 'PASS';
        if (det.status === 'PASS' || det.status === 'REVIEW' || det.status === 'REJECT') {
            return det.status;
        }

        const rawSpoilage = Number(det.spoilage_score);
        const spoilage = Number.isFinite(rawSpoilage) ? Math.max(0, Math.min(100, rawSpoilage)) : 0;

        let rawSim = Number(det.similarity_rate);
        let sim = Number.isFinite(rawSim) ? rawSim : 1.0;
        if (sim > 1.0) sim = sim / 100.0;
        sim = Math.max(0, Math.min(1.0, sim));

        if (spoilage >= 50 || sim < 0.35) return 'REJECT';
        if (spoilage >= 25 || sim < 0.60) return 'REVIEW';
        return 'PASS';
    }

    let validDetections = $derived(Array.isArray(detections) ? detections.filter((d): d is FoodDetection => Boolean(d)) : []);
    let passCount = $derived(validDetections.filter(d => getItemDecision(d) === 'PASS').length);
    let reviewCount = $derived(validDetections.filter(d => getItemDecision(d) === 'REVIEW').length);
    let rejectCount = $derived(validDetections.filter(d => getItemDecision(d) === 'REJECT').length);

    let displayedDetections = $derived.by(() => {
        if (filterMode === 'ALL') return validDetections;
        return validDetections.filter(d => getItemDecision(d) === filterMode);
    });

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
        <div class="header-filter-tabs" role="tablist" aria-label="Detection Filter Tabs">
            <button
                type="button"
                role="tab"
                class="filter-pill-btn"
                class:active={filterMode === 'ALL'}
                onclick={() => (filterMode = 'ALL')}
            >
                <span>All</span>
                <span class="count-badge">{validDetections.length}</span>
            </button>
            <button
                type="button"
                role="tab"
                class="filter-pill-btn tab-pass"
                class:active={filterMode === 'PASS'}
                onclick={() => (filterMode = 'PASS')}
                title="Filter Pass (Compliant) detections"
            >
                <span class="status-dot dot-pass"></span>
                <span>Pass Detection</span>
                <span class="count-badge badge-pass">{passCount}</span>
            </button>
            <button
                type="button"
                role="tab"
                class="filter-pill-btn tab-review"
                class:active={filterMode === 'REVIEW'}
                onclick={() => (filterMode = 'REVIEW')}
                title="Filter Review warnings"
            >
                <span class="status-dot dot-review"></span>
                <span>Review</span>
                <span class="count-badge badge-review">{reviewCount}</span>
            </button>
            <button
                type="button"
                role="tab"
                class="filter-pill-btn tab-reject"
                class:active={filterMode === 'REJECT'}
                onclick={() => (filterMode = 'REJECT')}
                title="Filter Rejected detections"
            >
                <span class="status-dot dot-reject"></span>
                <span>Reject</span>
                <span class="count-badge badge-reject">{rejectCount}</span>
            </button>
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
                {:else if displayedDetections.length === 0}
                    <tr>
                        <td colspan="5" class="empty-row">
                            <span class="empty-msg">No items matching "{filterMode}" found.</span>
                            <button type="button" class="reset-table-filter-btn" onclick={() => (filterMode = 'ALL')}>Show All Items</button>
                        </td>
                    </tr>
                {:else}
                    {#each displayedDetections as det, idx}
                        {@const decision = getItemDecision(det)}
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
                                {#if decision === 'REJECT'}
                                    <span class="badge reject">Reject</span>
                                {:else if decision === 'REVIEW'}
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

    .header-filter-tabs {
        display: flex;
        background: #f1f5f9;
        padding: 3px;
        border-radius: var(--radius-sm);
        border: 1px solid #e2e8f0;
        gap: 4px;
        flex-wrap: wrap;
    }

    .filter-pill-btn {
        display: inline-flex;
        align-items: center;
        gap: 5px;
        padding: 4px 10px;
        border-radius: var(--radius-sm);
        font-size: 0.72rem;
        font-weight: 600;
        color: #64748b;
        background: transparent;
        border: none;
        cursor: pointer;
        transition: all 0.15s ease;
        white-space: nowrap;
    }

    .filter-pill-btn:hover {
        background: rgba(255, 255, 255, 0.7);
        color: #1e293b;
    }

    .filter-pill-btn.active {
        background: #ffffff;
        color: #0f172a;
        box-shadow: 0 1px 3px rgba(0, 0, 0, 0.08);
        font-weight: 700;
    }

    .filter-pill-btn.tab-pass.active {
        color: #16a34a;
    }

    .filter-pill-btn.tab-review.active {
        color: #d97706;
    }

    .filter-pill-btn.tab-reject.active {
        color: #dc2626;
    }

    .status-dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
        display: inline-block;
    }

    .dot-pass {
        background: #16a34a;
    }

    .dot-review {
        background: #f59e0b;
    }

    .dot-reject {
        background: #ef4444;
    }

    .count-badge {
        font-size: 0.65rem;
        padding: 1px 5px;
        border-radius: 10px;
        background: rgba(148, 163, 184, 0.2);
        color: #475569;
        line-height: 1.1;
    }

    .badge-pass {
        background: rgba(22, 163, 74, 0.15);
        color: #16a34a;
    }

    .badge-review {
        background: rgba(245, 158, 11, 0.15);
        color: #d97706;
    }

    .badge-reject {
        background: rgba(239, 68, 68, 0.15);
        color: #dc2626;
    }

    .reset-table-filter-btn {
        margin-left: 10px;
        padding: 3px 8px;
        font-size: 0.7rem;
        font-weight: 600;
        background: #ffffff;
        color: #3679a8;
        border: 1px solid #cbd5e1;
        border-radius: var(--radius-sm);
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .reset-table-filter-btn:hover {
        background: #3679a8;
        color: #ffffff;
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
