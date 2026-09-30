<script lang="ts">
    import { onMount } from 'svelte';
    import { fetchRecords } from '../api';
    import type { InspectionRecord } from '../types';

    let records = $state<InspectionRecord[]>([]);
    let isLoading = $state(true);
    let searchTerm = $state('');
    let selectedRecord = $state<InspectionRecord | null>(null);

    async function loadData() {
        isLoading = true;
        records = await fetchRecords();
        isLoading = false;
    }

    onMount(() => {
        loadData();
    });

    let filteredRecords = $derived(
        records.filter(r =>
            r.id.toLowerCase().includes(searchTerm.toLowerCase()) ||
            r.device_id.toLowerCase().includes(searchTerm.toLowerCase()) ||
            (r.inspection_foods && r.inspection_foods.some(f => f.food_item?.name.toLowerCase().includes(searchTerm.toLowerCase())))
        )
    );
</script>

<div class="result-log-container">
    <div class="view-header">
        <div>
            <h2>Result Log</h2>
            <p class="muted">Historical Meal Inspection Records & Food Classifications Stored in PostgreSQL</p>
        </div>
        <div class="header-controls">
            <input
                type="text"
                placeholder="Search ID, Device, or Food item..."
                bind:value={searchTerm}
                class="search-input"
            />
            <button type="button" class="refresh-btn" onclick={loadData}>
                ↻ Refresh
            </button>
        </div>
    </div>

    <!-- Records Table -->
    <div class="table-card">
        <table class="styled-table">
            <thead>
                <tr>
                    <th>INSPECTION ID</th>
                    <th>DEVICE ID</th>
                    <th>TIMESTAMP</th>
                    <th>DETECTED FOODS</th>
                    <th>TEMP / HUM</th>
                    <th>VOC GAS</th>
                    <th>STATUS</th>
                    <th>ACTION</th>
                </tr>
            </thead>
            <tbody>
                {#if isLoading}
                    <tr><td colspan="8" class="notice-cell">Loading PostgreSQL inspection logs...</td></tr>
                {:else if filteredRecords.length === 0}
                    <tr><td colspan="8" class="notice-cell">No inspection records found.</td></tr>
                {:else}
                    {#each filteredRecords as rec}
                        <tr>
                            <td class="id-cell">
                                <span class="id-tag">#{rec.id.slice(0, 8)}</span>
                            </td>
                            <td class="device-cell">{rec.device_id}</td>
                            <td class="time-cell">{new Date(rec.created_at).toLocaleString()}</td>
                            <td class="foods-cell">
                                {#if rec.inspection_foods && rec.inspection_foods.length > 0}
                                    <div class="food-pill-group">
                                        {#each rec.inspection_foods as f}
                                            <span class="food-pill">{f.food_item?.name || 'Food'} ({f.weight_grams}g)</span>
                                        {/each}
                                    </div>
                                {:else}
                                    <span class="muted-text">Standard Tray</span>
                                {/if}
                            </td>
                            <td class="env-cell">
                                <strong>{rec.temperature}°C</strong> / <small>{rec.humidity}%</small>
                            </td>
                            <td class="voc-cell">
                                <span class="voc-val">{rec.gas_voc} kΩ</span>
                            </td>
                            <td class="status-cell">
                                {#if rec.is_spoiled}
                                    <span class="badge spoiled">REJECT</span>
                                {:else}
                                    <span class="badge passed">PASS</span>
                                {/if}
                            </td>
                            <td class="action-cell">
                                <button type="button" class="view-btn" onclick={() => (selectedRecord = rec)}>
                                    View Details
                                </button>
                            </td>
                        </tr>
                    {/each}
                {/if}
            </tbody>
        </table>
    </div>

    <!-- Inspection Details Modal -->
    {#if selectedRecord}
        <div class="modal-backdrop" role="dialog" aria-modal="true">
            <button
                type="button"
                class="backdrop-dismiss"
                aria-label="Close dialog"
                onclick={() => (selectedRecord = null)}
            ></button>
            <div class="modal-content">
                <div class="modal-header">
                    <h3>Inspection Audit #{selectedRecord.id}</h3>
                    <button type="button" class="close-btn" onclick={() => (selectedRecord = null)}>✕</button>
                </div>
                <div class="modal-body">
                    <pre class="json-viewer">{JSON.stringify(selectedRecord, null, 2)}</pre>
                </div>
            </div>
        </div>
    {/if}
</div>

<style>
    .result-log-container {
        display: flex;
        flex-direction: column;
        gap: 16px;
    }

    .view-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 12px;
        border-bottom: 1px solid var(--border-color, #1f293d);
        flex-wrap: wrap;
        gap: 12px;
    }

    .view-header h2 {
        margin: 0;
        font-size: 1.3rem;
        color: #ffffff;
    }

    .muted {
        margin: 4px 0 0;
        font-size: 0.8rem;
        color: #64748b;
    }

    .header-controls {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .search-input {
        background: #0b0f19;
        border: 1px solid #1e293b;
        color: #f1f5f9;
        font-size: 0.8rem;
        padding: 8px 12px;
        border-radius: 6px;
        width: 240px;
    }

    .refresh-btn {
        background: #141b2d;
        border: 1px solid #23304a;
        color: #00f0ff;
        font-size: 0.8rem;
        font-weight: 700;
        padding: 8px 14px;
        border-radius: 6px;
        cursor: pointer;
    }

    .table-card {
        background: var(--bg-panel, #121824);
        border: 1px solid var(--border-color, #1f293d);
        border-radius: 8px;
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
        font-size: 0.65rem;
        font-weight: 700;
        letter-spacing: 0.05em;
        padding: 10px 14px;
        border-bottom: 1px solid #1e293b;
        white-space: nowrap;
    }

    .styled-table td {
        padding: 12px 14px;
        border-bottom: 1px solid rgba(255, 255, 255, 0.04);
        white-space: nowrap;
        vertical-align: middle;
    }

    .id-tag {
        font-family: monospace;
        font-weight: 700;
        color: #00f0ff;
        background: rgba(0, 240, 255, 0.1);
        padding: 3px 6px;
        border-radius: 4px;
    }

    .food-pill-group {
        display: flex;
        gap: 6px;
        flex-wrap: wrap;
    }

    .food-pill {
        font-size: 0.7rem;
        padding: 2px 6px;
        background: rgba(59, 130, 246, 0.15);
        color: #93c5fd;
        border-radius: 4px;
        border: 1px solid rgba(59, 130, 246, 0.3);
    }

    .badge {
        font-size: 0.7rem;
        font-weight: 800;
        padding: 3px 8px;
        border-radius: 4px;
    }

    .badge.passed {
        background: #10b981;
        color: #000;
    }

    .badge.spoiled {
        background: #ef4444;
        color: #ffffff;
    }

    .view-btn {
        background: #141b2d;
        border: 1px solid #23304a;
        color: #cbd5e1;
        font-size: 0.7rem;
        padding: 4px 10px;
        border-radius: 4px;
        cursor: pointer;
    }

    .view-btn:hover {
        background: #1e293b;
        color: #ffffff;
    }

    .modal-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.7);
        backdrop-filter: blur(4px);
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 100;
    }

    .backdrop-dismiss {
        position: absolute;
        inset: 0;
        background: transparent;
        border: none;
        cursor: pointer;
        width: 100%;
        height: 100%;
    }

    .modal-content {
        position: relative;
        z-index: 101;
        background: #0f172a;
        border: 1px solid #334155;
        border-radius: 8px;
        width: 600px;
        max-width: 90vw;
        max-height: 80vh;
        display: flex;
        flex-direction: column;
        box-shadow: 0 20px 40px rgba(0, 0, 0, 0.6);
    }

    .modal-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 14px 18px;
        border-bottom: 1px solid #1e293b;
    }

    .modal-header h3 {
        margin: 0;
        font-size: 0.95rem;
        color: #f1f5f9;
        font-family: monospace;
    }

    .close-btn {
        background: transparent;
        border: none;
        color: #64748b;
        font-size: 1.1rem;
        cursor: pointer;
    }

    .modal-body {
        padding: 16px;
        overflow-y: auto;
    }

    .json-viewer {
        background: #080c14;
        border: 1px solid #1e293b;
        border-radius: 6px;
        padding: 12px;
        color: #00f0ff;
        font-family: monospace;
        font-size: 0.75rem;
        overflow-x: auto;
    }

    .notice-cell {
        text-align: center;
        padding: 24px;
        color: #64748b;
    }
</style>
