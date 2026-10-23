<script lang="ts">
    import { onMount } from 'svelte';
    import { fetchInspectionAuditRecords } from '../api';
    import type { InspectionAuditRecord } from '../types';
    import {
        copyMarkdown,
        downloadMarkdown,
        generateBatchInspectionMarkdown,
        generateInspectionMarkdown
    } from '../utils/reportGenerator';

    const MAX_BATCH_ZIP_BYTES = 15 * 1024 * 1024;

    interface BatchZipSummary {
        batch_id: string;
        total_trays: number;
        accepted: number;
        rejected: number;
        processing_time_ms: number;
        pdf_download_url: string;
        error?: string;
    }

    let records = $state<InspectionAuditRecord[]>([]);
    let isLoading = $state(true);
    let isDownloadingPdf = $state<Record<number, boolean>>({});
    let isUploadingBatch = $state(false);
    let zipInput = $state<HTMLInputElement | null>(null);
    let selectedRecord = $state<InspectionAuditRecord | null>(null);
    let failedImages = $state<Record<string, boolean>>({});
    let toast = $state('');
    let toastTimer: ReturnType<typeof setTimeout> | null = null;

    async function loadRecords() {
        const next = await fetchInspectionAuditRecords(100);
        records = next;
        isLoading = false;
        if (selectedRecord) {
            const updated = next.find((record) => record.frame_id === selectedRecord?.frame_id);
            if (updated) selectedRecord = updated;
        }
    }

    onMount(() => {
        loadRecords();
        const timer = setInterval(loadRecords, 3000);
        return () => clearInterval(timer);
    });

    function showToast(message: string, duration = 2600) {
        toast = message;
        if (toastTimer) clearTimeout(toastTimer);
        toastTimer = setTimeout(() => {
            toast = '';
        }, duration);
    }

    function imageFailed(path: string) {
        if (!path) return;
        failedImages = { ...failedImages, [path]: true };
    }

    function isVerdictRejected(record: InspectionAuditRecord): boolean {
        return record.verdict === 'REJECTED';
    }

    function formatTimestamp(value: string): string {
        const parsed = new Date(value);
        return Number.isNaN(parsed.getTime()) ? value : parsed.toLocaleString();
    }

    function reportRecord(record: InspectionAuditRecord) {
        const markdown = generateInspectionMarkdown(record);
        downloadMarkdown(`MBG-REPORT-${record.frame_id}.md`, markdown);
        showToast(`Report for frame #${record.frame_id} downloaded`);
    }

    function reportBatch() {
        if (records.length === 0) {
            showToast('No inspection records to export');
            return;
        }
        const markdown = generateBatchInspectionMarkdown(records);
        downloadMarkdown(`MBG-REPORT-BATCH-${records.length}.md`, markdown);
        showToast(`Batch report (${records.length} records) downloaded`);
    }

    function openZipPicker() {
        if (isUploadingBatch) return;
        zipInput?.click();
    }

    async function downloadBatchArtifact(url: string, batchId: string) {
        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`Batch report download failed (${response.status})`);
        }

        // The backend serves the LaTeX source when pdflatex is unavailable;
        // X-Report-Format tells us which artifact actually arrived.
        const servedFormat = response.headers.get('X-Report-Format') === 'tex' ? 'tex' : 'pdf';
        const blob = await response.blob();
        const blobUrl = window.URL.createObjectURL(blob);
        const anchor = document.createElement('a');
        anchor.href = blobUrl;
        anchor.download = `BawalVision-Batch-${batchId}.${servedFormat}`;
        document.body.appendChild(anchor);
        anchor.click();
        document.body.removeChild(anchor);
        window.URL.revokeObjectURL(blobUrl);

        showToast(servedFormat === 'pdf'
            ? `Master batch PDF ${batchId} downloaded`
            : `TeX engine unavailable — batch LaTeX source ${batchId} downloaded`);
    }

    async function handleZipSelected(event: Event) {
        const input = event.currentTarget as HTMLInputElement;
        const file = input.files?.[0];
        input.value = '';
        if (!file) return;

        if (!file.name.toLowerCase().endsWith('.zip')) {
            showToast('Invalid file: please select a .zip archive');
            return;
        }
        if (file.size > MAX_BATCH_ZIP_BYTES) {
            showToast('ZIP exceeds the 15 MB upload limit');
            return;
        }

        isUploadingBatch = true;
        showToast('Uploading & processing batch ZIP with OpenMP AI...', 10000);
        try {
            const response = await fetch('/api/v1/inspections/batch-zip', {
                method: 'POST',
                headers: { 'Content-Type': 'application/zip' },
                body: file
            });

            const payload = (await response.json().catch(() => null)) as BatchZipSummary | null;
            if (!response.ok) {
                throw new Error(payload?.error ?? `Server returned ${response.status}`);
            }

            const trays = payload?.total_trays ?? 0;
            const elapsedMs = Math.round(payload?.processing_time_ms ?? 0);
            showToast(`Batch processed: ${trays} trays in ${elapsedMs} ms. Downloading Master PDF...`, 10000);

            if (payload?.pdf_download_url && payload.batch_id) {
                await downloadBatchArtifact(payload.pdf_download_url, payload.batch_id);
            }
        } catch (err) {
            showToast(err instanceof Error ? err.message : 'Batch ZIP upload failed', 5000);
        } finally {
            isUploadingBatch = false;
        }
    }

    async function copyRecord(record: InspectionAuditRecord) {
        const markdown = generateInspectionMarkdown(record);
        const copied = await copyMarkdown(markdown);
        showToast(copied ? 'Markdown copied to clipboard' : 'Clipboard copy failed');
    }

    async function copySelected() {
        if (!selectedRecord) return;
        const markdown = generateInspectionMarkdown(selectedRecord);
        const copied = await copyMarkdown(markdown);
        showToast(copied ? 'Markdown copied to clipboard' : 'Clipboard copy failed');
    }

    function reportSelected() {
        if (!selectedRecord) return;
        reportRecord(selectedRecord);
    }

    async function downloadBackendReport(frameId: number, format: 'pdf' | 'tex') {
        try {
            if (format === 'pdf') {
                isDownloadingPdf = { ...isDownloadingPdf, [frameId]: true };
                showToast(`Compiling LaTeX PDF for frame #${frameId}...`);
            }

            const endpoint = `/api/v1/inspections/${frameId}/report.${format}`;
            const response = await fetch(endpoint);

            if (!response.ok) {
                throw new Error(`Server returned ${response.status}`);
            }

            // The backend falls back to the .tex source when pdflatex is not
            // installed; X-Report-Format tells us which artifact we received.
            const servedFormat = response.headers.get('X-Report-Format') === 'pdf' ? 'pdf' : 'tex';

            const blob = await response.blob();
            const blobUrl = window.URL.createObjectURL(blob);
            const anchor = document.createElement('a');
            anchor.href = blobUrl;
            anchor.download = `BawalVision-Report-${frameId}.${servedFormat}`;
            document.body.appendChild(anchor);
            anchor.click();
            document.body.removeChild(anchor);
            window.URL.revokeObjectURL(blobUrl);

            showToast(servedFormat === 'pdf'
                ? `PDF report #${frameId} ready`
                : `TeX engine unavailable — LaTeX source #${frameId} downloaded`);
        } catch (err) {
            showToast(`Failed to export ${format.toUpperCase()} report`);
        } finally {
            if (format === 'pdf') {
                isDownloadingPdf = { ...isDownloadingPdf, [frameId]: false };
            }
        }
    }
</script>

<div class="audit-panel card">
    <div class="audit-header">
        <div class="audit-title-wrap">
            <span class="audit-heading">Inspection Audit Stream</span>
            <span class="audit-count">{records.length} records</span>
            <span class="buffer-note">50-slot rolling image buffer</span>
        </div>
        <div class="audit-actions">
            <button type="button" class="action-btn" onclick={loadRecords}>↻ Refresh</button>
            <button type="button" class="action-btn primary" onclick={reportBatch}>📄 Export Batch (.md)</button>
            <button
                type="button"
                class="action-btn batch-upload-btn"
                disabled={isUploadingBatch}
                onclick={openZipPicker}
            >
                {isUploadingBatch ? '⏳ Processing ZIP...' : '📦 Upload Batch ZIP'}
            </button>
            <input
                type="file"
                accept=".zip,application/zip"
                class="hidden-file-input"
                aria-label="Upload batch ZIP archive"
                bind:this={zipInput}
                onchange={handleZipSelected}
            />
        </div>
    </div>

    {#if isLoading}
        <div class="audit-empty">Loading inspection audit records...</div>
    {:else if records.length === 0}
        <div class="audit-empty">
            No inspection frames captured yet. Stream a frame to the Crow backend to populate the audit log.
        </div>
    {:else}
        <div class="audit-grid">
            {#each records as record (record.frame_id + '-' + record.image_path)}
                {@const detections = record.detections ?? []}
                <article class="audit-card">
                    <div class="thumb-wrap">
                        {#if record.image_path && !failedImages[record.image_path]}
                            <img
                                src={record.image_path}
                                alt={`Inspection ${record.frame_id}`}
                                loading="lazy"
                                onerror={() => imageFailed(record.image_path)}
                            />
                        {:else}
                            <div class="thumb-fallback">
                                <span class="fallback-icon">🗂️</span>
                                <span>Snapshot Rolled Out</span>
                            </div>
                        {/if}
                        <span class="verdict-badge" class:rejected={isVerdictRejected(record)}>
                            {record.verdict}
                        </span>
                    </div>

                    <div class="audit-card-body">
                        <div class="audit-meta-row">
                            <strong class="frame-label">Frame #{record.frame_id}</strong>
                            <span class="timestamp-label">{formatTimestamp(record.timestamp)}</span>
                        </div>

                        <div class="audit-metric-row">
                            <span class="metric-chip">🌡️ {record.surface_temp > 0 ? `${record.surface_temp.toFixed(1)} °C` : 'N/A'}</span>
                            <span class="metric-chip">⏳ t_safe {record.t_safe_hours > 0 ? `${record.t_safe_hours.toFixed(1)} h` : 'N/A'}</span>
                            <span class="metric-chip">{detections.length} detections</span>
                        </div>

                        <div class="detection-log">
                            {#if detections.length === 0}
                                <span class="no-detection">No food item detected in this frame.</span>
                            {:else}
                                {#each detections.slice(0, 4) as detection}
                                    <div class="detection-line">
                                        <span class="class-name">{detection.class_name}</span>
                                        <span class="det-stat">conf {(detection.confidence * 100).toFixed(0)}%</span>
                                        <span class="det-stat">fresh {(detection.freshness_score * 100).toFixed(0)}%</span>
                                        <span class="det-stat">portion {(detection.portion_ratio * 100).toFixed(0)}%</span>
                                    </div>
                                {/each}
                                {#if detections.length > 4}
                                    <span class="more-detections">+{detections.length - 4} more items</span>
                                {/if}
                            {/if}
                        </div>

                        <div class="audit-card-actions">
                            <button
                                type="button"
                                class="action-btn pdf-btn"
                                disabled={isDownloadingPdf[record.frame_id]}
                                onclick={() => downloadBackendReport(record.frame_id, 'pdf')}
                            >
                                {isDownloadingPdf[record.frame_id] ? '⏳ Compiling...' : '📑 PDF (LaTeX)'}
                            </button>
                            <button
                                type="button"
                                class="action-btn"
                                onclick={() => downloadBackendReport(record.frame_id, 'tex')}
                            >
                                Source (.tex)
                            </button>
                            <button type="button" class="action-btn" onclick={() => reportRecord(record)}>
                                📄 Markdown (.md)
                            </button>
                            <button type="button" class="action-btn" onclick={() => copyRecord(record)}>
                                📋 Copy Markdown
                            </button>
                            <button type="button" class="action-btn ghost" onclick={() => (selectedRecord = record)}>
                                Details
                            </button>
                        </div>
                    </div>
                </article>
            {/each}
        </div>
    {/if}
</div>

{#if selectedRecord}
    {@const detections = selectedRecord.detections ?? []}
    <div class="modal-backdrop" role="dialog" aria-modal="true">
        <button
            type="button"
            class="modal-scrim"
            aria-label="Close inspection detail"
            onclick={() => (selectedRecord = null)}
        ></button>
        <div class="modal-card">
            <div class="modal-header">
                <div>
                    <h3>Inspection Audit #{selectedRecord.frame_id}</h3>
                    <span class="modal-time">{formatTimestamp(selectedRecord.timestamp)}</span>
                </div>
                <button type="button" class="modal-close" onclick={() => (selectedRecord = null)}>✕</button>
            </div>

            <div class="modal-body">
                <div class="modal-evidence">
                    {#if selectedRecord.image_path && !failedImages[selectedRecord.image_path]}
                        <img
                            src={selectedRecord.image_path}
                            alt={`Inspection ${selectedRecord.frame_id}`}
                            onerror={() => imageFailed(selectedRecord?.image_path ?? '')}
                        />
                    {:else}
                        <div class="thumb-fallback large">
                            <span class="fallback-icon">🗂️</span>
                            <span>Snapshot Rolled Out of the 50-slot buffer</span>
                        </div>
                    {/if}
                </div>

                <div class="modal-verdict" class:rejected={isVerdictRejected(selectedRecord)}>
                    {selectedRecord.verdict}
                </div>

                <div class="modal-metrics">
                    <div class="modal-metric">
                        <span class="label">Surface Temp (GY-906)</span>
                        <strong>{selectedRecord.surface_temp > 0 ? `${selectedRecord.surface_temp.toFixed(1)} °C` : 'N/A'}</strong>
                    </div>
                    <div class="modal-metric">
                        <span class="label">Safe Window (t_safe)</span>
                        <strong>{selectedRecord.t_safe_hours > 0 ? `${selectedRecord.t_safe_hours.toFixed(1)} hours` : 'N/A'}</strong>
                    </div>
                    <div class="modal-metric">
                        <span class="label">Detections</span>
                        <strong>{detections.length}</strong>
                    </div>
                </div>

                <h4 class="section-heading">Detection Breakdown</h4>
                <div class="modal-table-wrap">
                    <table class="detection-table">
                        <thead>
                            <tr>
                                <th>No</th>
                                <th>Item / Kompartemen</th>
                                <th>Bounding Box [x, y, w, h]</th>
                                <th>Confidence</th>
                                <th>Freshness</th>
                                <th>Estimasi Porsi</th>
                            </tr>
                        </thead>
                        <tbody>
                            {#if detections.length === 0}
                                <tr>
                                    <td colspan="6" class="empty-cell">No food item detected in this frame.</td>
                                </tr>
                            {:else}
                                {#each detections as detection, index}
                                    <tr>
                                        <td>{index + 1}</td>
                                        <td>{detection.class_name}</td>
                                        <td class="mono">[{detection.bbox[0]}, {detection.bbox[1]}, {detection.bbox[2]}, {detection.bbox[3]}]</td>
                                        <td>{(detection.confidence * 100).toFixed(1)}%</td>
                                        <td>{(detection.freshness_score * 100).toFixed(1)}%</td>
                                        <td>{(detection.portion_ratio * 100).toFixed(1)}%</td>
                                    </tr>
                                {/each}
                            {/if}
                        </tbody>
                    </table>
                </div>

                <h4 class="section-heading">Audit Integrity</h4>
                <code class="checksum">{selectedRecord.checksum_sha256 || 'N/A'}</code>

                <div class="modal-actions">
                    <button
                        type="button"
                        class="action-btn pdf-btn"
                        disabled={isDownloadingPdf[selectedRecord.frame_id]}
                        onclick={() => downloadBackendReport(selectedRecord?.frame_id ?? 0, 'pdf')}
                    >
                        {isDownloadingPdf[selectedRecord.frame_id] ? '⏳ Compiling PDF...' : '📑 Export PDF (LaTeX)'}
                    </button>
                    <button
                        type="button"
                        class="action-btn"
                        onclick={() => downloadBackendReport(selectedRecord?.frame_id ?? 0, 'tex')}
                    >
                        LaTeX (.tex)
                    </button>
                    <button type="button" class="action-btn primary" onclick={reportSelected}>
                        📄 Download Markdown (.md)
                    </button>
                    <button type="button" class="action-btn" onclick={copySelected}>
                        📋 Copy Markdown
                    </button>
                </div>
            </div>
        </div>
    </div>
{/if}

{#if toast}
    <div class="audit-toast" role="status">{toast}</div>
{/if}

<style>
    .audit-panel {
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        box-shadow: var(--shadow-card);
        padding: 14px 16px;
        display: flex;
        flex-direction: column;
        gap: 12px;
    }

    .audit-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        flex-wrap: wrap;
        padding-bottom: 10px;
        border-bottom: 1px solid var(--border-subtle);
    }

    .audit-title-wrap {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
    }

    .audit-heading {
        font-size: 0.82rem;
        font-weight: 800;
        letter-spacing: 0.05em;
        text-transform: uppercase;
        color: var(--text-primary);
    }

    .audit-count {
        font-size: 0.68rem;
        font-weight: 700;
        color: var(--accent-blue);
        background: var(--accent-blue-dim);
        border: 1px solid #c7dfef;
        border-radius: var(--radius-pill);
        padding: 2px 8px;
    }

    .buffer-note {
        font-size: 0.64rem;
        color: var(--text-muted);
        font-family: var(--font-mono);
    }

    .audit-actions,
    .audit-card-actions,
    .modal-actions {
        display: flex;
        align-items: center;
        gap: 6px;
        flex-wrap: wrap;
    }

    .audit-card-actions {
        margin-top: auto;
        padding-top: 8px;
        border-top: 1px solid var(--border-subtle);
    }

    .audit-card-actions .action-btn {
        flex: 1 1 auto;
        text-align: center;
        justify-content: center;
    }

    .action-btn {
        background: var(--bg-chip);
        border: 1px solid var(--border-subtle);
        color: var(--text-secondary);
        font-size: 0.64rem;
        font-weight: 700;
        padding: 5px 8px;
        border-radius: var(--radius-sm);
        cursor: pointer;
        transition: all 0.12s ease;
        display: inline-flex;
        align-items: center;
        gap: 4px;
    }

    .action-btn:hover:not(:disabled) {
        background: var(--accent-blue-dim);
        border-color: var(--accent-blue-light);
        color: var(--accent-blue);
    }

    .action-btn:disabled {
        opacity: 0.6;
        cursor: not-allowed;
    }

    .action-btn.pdf-btn {
        background: #047857;
        border-color: #059669;
        color: #ffffff;
    }

    .action-btn.pdf-btn:hover:not(:disabled) {
        background: #059669;
        border-color: #10b981;
        color: #ffffff;
    }

    .action-btn.primary {
        background: var(--accent-blue);
        border-color: var(--accent-blue);
        color: var(--text-white);
    }

    .action-btn.primary:hover:not(:disabled) {
        background: var(--accent-blue-light);
    }

    .action-btn.ghost {
        background: transparent;
    }

    .action-btn.batch-upload-btn {
        background: var(--accent-blue-dim);
        border-color: var(--accent-blue-light);
        color: var(--accent-blue);
    }

    .action-btn.batch-upload-btn:hover:not(:disabled) {
        background: var(--accent-blue);
        border-color: var(--accent-blue);
        color: var(--text-white);
    }

    .hidden-file-input {
        display: none;
    }

    .audit-empty {
        padding: 24px 12px;
        text-align: center;
        color: var(--text-muted);
        font-size: 0.78rem;
    }

    .audit-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
        gap: 12px;
    }

    .audit-card {
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        background: var(--bg-card);
        overflow: hidden;
        display: flex;
        flex-direction: column;
    }

    .thumb-wrap {
        position: relative;
        aspect-ratio: 4 / 3;
        background: var(--bg-inset);
        border-bottom: 1px solid var(--border-subtle);
    }

    .thumb-wrap img,
    .modal-evidence img {
        width: 100%;
        height: 100%;
        object-fit: cover;
        display: block;
    }

    .thumb-fallback {
        width: 100%;
        height: 100%;
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        gap: 6px;
        color: var(--text-dim);
        font-size: 0.68rem;
        text-align: center;
        padding: 12px;
    }

    .thumb-fallback.large {
        min-height: 220px;
        font-size: 0.78rem;
    }

    .fallback-icon {
        font-size: 1.5rem;
    }

    .verdict-badge {
        position: absolute;
        top: 8px;
        right: 8px;
        font-size: 0.6rem;
        font-weight: 800;
        letter-spacing: 0.05em;
        padding: 3px 7px;
        border-radius: var(--radius-pill);
        background: var(--accent-green);
        color: var(--text-white);
        box-shadow: var(--shadow-sm);
    }

    .verdict-badge.rejected {
        background: var(--accent-red);
    }

    .audit-card-body {
        padding: 10px;
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .audit-meta-row {
        display: flex;
        align-items: baseline;
        justify-content: space-between;
        gap: 8px;
    }

    .frame-label {
        font-size: 0.76rem;
        color: var(--text-primary);
    }

    .timestamp-label {
        font-size: 0.62rem;
        color: var(--text-muted);
        font-family: var(--font-mono);
        text-align: right;
    }

    .audit-metric-row {
        display: flex;
        gap: 5px;
        flex-wrap: wrap;
    }

    .metric-chip {
        font-size: 0.6rem;
        font-family: var(--font-mono);
        color: var(--text-secondary);
        background: var(--bg-chip);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-pill);
        padding: 2px 6px;
    }

    .detection-log {
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
        padding: 7px 8px;
        display: flex;
        flex-direction: column;
        gap: 4px;
        min-height: 54px;
    }

    .detection-line {
        display: flex;
        align-items: baseline;
        gap: 6px;
        font-size: 0.62rem;
        font-family: var(--font-mono);
        color: var(--text-secondary);
        flex-wrap: wrap;
    }

    .class-name {
        color: var(--text-primary);
        font-weight: 700;
        min-width: 48px;
    }

    .det-stat {
        color: var(--text-muted);
    }

    .no-detection,
    .more-detections {
        font-size: 0.62rem;
        color: var(--text-muted);
        font-style: italic;
    }

    .modal-backdrop {
        position: fixed;
        inset: 0;
        z-index: 60;
        display: flex;
        align-items: center;
        justify-content: center;
        padding: 18px;
    }

    .modal-scrim {
        position: absolute;
        inset: 0;
        border: 0;
        background: rgba(15, 23, 42, 0.45);
        cursor: pointer;
    }

    .modal-card {
        position: relative;
        z-index: 1;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        box-shadow: 0 18px 50px rgba(15, 23, 42, 0.25);
        width: min(920px, 100%);
        max-height: 88vh;
        overflow-y: auto;
    }

    .modal-header {
        display: flex;
        align-items: flex-start;
        justify-content: space-between;
        gap: 12px;
        padding: 14px 16px;
        border-bottom: 1px solid var(--border-subtle);
        position: sticky;
        top: 0;
        background: var(--bg-card);
        z-index: 2;
    }

    .modal-header h3 {
        margin: 0;
        font-size: 0.92rem;
        color: var(--text-primary);
    }

    .modal-time {
        font-size: 0.66rem;
        color: var(--text-muted);
        font-family: var(--font-mono);
    }

    .modal-close {
        border: 1px solid var(--border-subtle);
        background: var(--bg-chip);
        color: var(--text-secondary);
        width: 28px;
        height: 28px;
        border-radius: var(--radius-sm);
        cursor: pointer;
    }

    .modal-body {
        padding: 14px 16px 18px;
        display: flex;
        flex-direction: column;
        gap: 12px;
    }

    .modal-evidence {
        width: 100%;
        max-height: 320px;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-md);
        overflow: hidden;
    }

    .modal-evidence img {
        max-height: 320px;
        object-fit: contain;
        background: #0f172a;
    }

    .modal-verdict {
        align-self: flex-start;
        font-size: 0.72rem;
        font-weight: 800;
        letter-spacing: 0.06em;
        padding: 5px 12px;
        border-radius: var(--radius-pill);
        background: var(--accent-green);
        color: var(--text-white);
    }

    .modal-verdict.rejected {
        background: var(--accent-red);
    }

    .modal-metrics {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
        gap: 8px;
    }

    .modal-metric {
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
        background: var(--bg-inset);
        padding: 8px 10px;
        display: flex;
        flex-direction: column;
        gap: 3px;
    }

    .modal-metric .label {
        font-size: 0.62rem;
        color: var(--text-muted);
        text-transform: uppercase;
        letter-spacing: 0.04em;
    }

    .modal-metric strong {
        font-size: 0.8rem;
        color: var(--text-primary);
    }

    .section-heading {
        margin: 4px 0 0;
        font-size: 0.7rem;
        text-transform: uppercase;
        letter-spacing: 0.05em;
        color: var(--text-secondary);
    }

    .modal-table-wrap {
        overflow-x: auto;
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
    }

    .detection-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 0.68rem;
    }

    .detection-table th,
    .detection-table td {
        padding: 6px 8px;
        text-align: left;
        border-bottom: 1px solid var(--border-subtle);
        white-space: nowrap;
    }

    .detection-table th {
        background: var(--bg-inset);
        color: var(--text-secondary);
        font-size: 0.6rem;
        text-transform: uppercase;
        letter-spacing: 0.04em;
    }

    .detection-table .mono {
        font-family: var(--font-mono);
    }

    .empty-cell {
        text-align: center;
        color: var(--text-muted);
        font-style: italic;
    }

    .checksum {
        display: block;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
        padding: 8px 10px;
        font-family: var(--font-mono);
        font-size: 0.64rem;
        color: var(--text-secondary);
        overflow-wrap: anywhere;
    }

    .audit-toast {
        position: fixed;
        right: 18px;
        bottom: 18px;
        z-index: 80;
        background: var(--accent-charcoal);
        color: var(--text-white);
        font-size: 0.72rem;
        font-weight: 600;
        padding: 9px 14px;
        border-radius: var(--radius-md);
        box-shadow: 0 10px 28px rgba(15, 23, 42, 0.3);
    }

    @media (max-width: 640px) {
        .audit-grid {
            grid-template-columns: 1fr;
        }

        .audit-header {
            align-items: flex-start;
        }
    }
</style>