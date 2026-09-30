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
    <div class="card-header-bar">
        <div class="header-left">
            <span class="view-icon">📜</span>
            <div>
                <h2>Inspection History & Database Archive</h2>
                <p class="sub-label">Historical Records Stored in PostgreSQL</p>
            </div>
        </div>
        <div class="header-controls">
            <input
                type="text"
                placeholder="Search Lot, Device, Food..."
                bind:value={searchTerm}
                class="search-input"
            />
            <button type="button" class="refresh-chip-btn" onclick={loadData}>
                ↻ Refresh DB
            </button>
        </div>
    </div>

    <!-- Records Table Card -->
    <div class="table-card">
        <table class="styled-table">
            <thead>
                <tr>
                    <th>Lot ID</th>
                    <th>Device</th>
                    <th>Timestamp</th>
                    <th>Detected Components</th>
                    <th>Temperature</th>
                    <th>CPU Load</th>
                    <th>Status</th>
                    <th>Action</th>
                </tr>
            </thead>
            <tbody>
                {#if isLoading}
                    <tr><td colspan="8" class="notice-cell">Connecting to PostgreSQL database...</td></tr>
                {:else if filteredRecords.length === 0}
                    <tr><td colspan="8" class="notice-cell">No matching inspection records found in storage.</td></tr>
                {:else}
                    {#each filteredRecords as rec}
                        <tr>
                            <td class="id-cell">
                                <span class="id-tag">#{rec.id.slice(0, 8).toUpperCase()}</span>
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
                                <strong>{rec.temperature}°C</strong>
                            </td>
                            <td class="cpu-cell">
                                <span class="cpu-val">{rec.cpu_load || 0}%</span>
                            </td>
                            <td class="status-cell">
                                {#if rec.is_spoiled}
                                    <span class="badge spoiled">Reject</span>
                                {:else}
                                    <span class="badge passed">Pass</span>
                                {/if}
                            </td>
                            <td class="action-cell">
                                <button type="button" class="view-btn" onclick={() => (selectedRecord = rec)}>
                                    Inspect JSON
                                </button>
                            </td>
                        </tr>
                    {/each}
                {/if}
            </tbody>
        </table>
    </div>

    <!-- Modal Viewer -->
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
                    <h3>Audit Record #{selectedRecord.id}</h3>
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

    .card-header-bar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding-bottom: 12px;
        border-bottom: 1px solid var(--border-subtle);
        flex-wrap: wrap;
        gap: 12px;
    }

    .header-left {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .view-icon {
        font-size: 1.5rem;
    }

    .header-left h2 {
        margin: 0;
        font-size: 1.15rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .sub-label {
        margin: 2px 0 0;
        font-size: 0.72rem;
        color: var(--text-muted);
    }

    .header-controls {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .search-input {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        color: var(--text-primary);
        font-family: var(--font-sans);
        font-size: 0.8rem;
        padding: 8px 14px;
        border-radius: var(--radius-pill);
        width: 240px;
        outline: none;
    }

    .search-input:focus {
        border-color: var(--accent-blue);
    }

    .refresh-chip-btn {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        color: var(--accent-blue);
        font-size: 0.8rem;
        font-weight: 600;
        padding: 8px 16px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .refresh-chip-btn:hover {
        background: var(--bg-card-hover);
        border-color: var(--border-highlight);
    }

    .table-card {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        overflow-x: auto;
        box-shadow: var(--shadow-card);
    }

    .styled-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 0.8rem;
        text-align: left;
    }

    .styled-table th {
        background: var(--bg-chip);
        color: var(--text-secondary);
        font-weight: 700;
        font-size: 0.72rem;
        padding: 12px 16px;
        border-bottom: 1px solid var(--border-subtle);
        white-space: nowrap;
    }

    .styled-table td {
        padding: 12px 16px;
        border-bottom: 1px solid var(--border-subtle);
        white-space: nowrap;
        vertical-align: middle;
    }

    .styled-table tr:last-child td {
        border-bottom: none;
    }

    .styled-table tr:hover {
        background: rgba(255, 255, 255, 0.02);
    }

    .id-tag {
        font-family: var(--font-mono);
        font-weight: 700;
        color: var(--accent-blue);
        background: var(--accent-blue-dim);
        padding: 2px 8px;
        border-radius: var(--radius-pill);
    }

    .device-cell {
        color: var(--text-secondary);
    }

    .time-cell {
        color: var(--text-muted);
        font-size: 0.72rem;
    }

    .food-pill-group {
        display: flex;
        gap: 6px;
        flex-wrap: wrap;
    }

    .food-pill {
        font-size: 0.68rem;
        padding: 2px 8px;
        background: var(--bg-chip);
        color: var(--text-primary);
        border-radius: var(--radius-pill);
    }

    .muted-text {
        color: var(--text-muted);
        font-size: 0.72rem;
    }

    .env-cell strong {
        color: var(--text-primary);
    }

    .cpu-val {
        color: var(--accent-blue);
        font-weight: 700;
        font-family: var(--font-mono);
    }

    .badge {
        font-size: 0.68rem;
        font-weight: 700;
        padding: 3px 10px;
        border-radius: var(--radius-pill);
    }

    .badge.passed {
        background: var(--accent-green-dim);
        color: var(--accent-green);
        border: 1px solid rgba(34, 197, 94, 0.3);
    }

    .badge.spoiled {
        background: var(--accent-red-dim);
        color: var(--accent-red);
        border: 1px solid rgba(244, 63, 94, 0.3);
    }

    .view-btn {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        color: var(--text-secondary);
        font-size: 0.72rem;
        font-weight: 600;
        padding: 4px 10px;
        border-radius: var(--radius-pill);
        cursor: pointer;
        transition: all 0.15s ease;
    }

    .view-btn:hover {
        background: var(--bg-card-hover);
        color: var(--text-primary);
    }

    .modal-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.75);
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
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        width: 600px;
        max-width: 90vw;
        max-height: 80vh;
        display: flex;
        flex-direction: column;
        box-shadow: 0 10px 30px rgba(0,0,0,0.6);
    }

    .modal-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 16px 20px;
        border-bottom: 1px solid var(--border-subtle);
    }

    .modal-header h3 {
        margin: 0;
        font-size: 0.95rem;
        color: var(--text-primary);
        font-weight: 700;
    }

    .close-btn {
        background: transparent;
        border: none;
        color: var(--text-muted);
        font-size: 1.1rem;
        cursor: pointer;
    }

    .close-btn:hover {
        color: var(--text-primary);
    }

    .modal-body {
        padding: 16px 20px;
        overflow-y: auto;
    }

    .json-viewer {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        padding: 14px;
        color: var(--accent-blue);
        font-family: var(--font-mono);
        font-size: 0.75rem;
        overflow-x: auto;
    }

    .notice-cell {
        text-align: center;
        padding: 28px;
        color: var(--text-muted);
        font-size: 0.8rem;
    }
</style>
