<script lang="ts">
    import { onDestroy } from 'svelte';

    const MAX_ZIP_BYTES = 15 * 1024 * 1024;
    const BATCH_ENDPOINT = '/api/v1/inspections/batch-zip';

    interface BatchDetection {
        class_name: string;
        confidence: number;
        freshness: number;
        bbox: number[];
    }

    interface BatchItem {
        index: number;
        filename: string;
        accepted: boolean;
        surface_temp: number;
        t_safe_hours: number;
        annotated_url: string;
        detections: BatchDetection[];
    }

    interface BatchSummary {
        status: string;
        batch_id: string;
        total_trays: number;
        accepted: number;
        rejected: number;
        processing_time_ms: number;
        pdf_download_url: string;
        items?: BatchItem[];
        error?: string;
    }

    type BatchStage = 'idle' | 'uploading' | 'processing' | 'compiling' | 'done' | 'error';

    const STAGES: { id: BatchStage; label: string }[] = [
        { id: 'uploading', label: 'Uploading ZIP' },
        { id: 'processing', label: 'Processing OpenMP Multi-Core' },
        { id: 'compiling', label: 'Compiling LaTeX Master PDF' }
    ];

    let stage = $state<BatchStage>('idle');
    let isDragging = $state(false);
    let fileInput = $state<HTMLInputElement | null>(null);
    let summary = $state<BatchSummary | null>(null);
    let errorMessage = $state('');
    let noticeMessage = $state('');
    let selectedFileName = $state('');
    let uploadBytes = $state(0);
    let elapsedMs = $state(0);
    let isDownloadingPdf = $state(false);
    let failedImages = $state<Record<string, boolean>>({});

    let stageTimerA: ReturnType<typeof setTimeout> | null = null;
    let stageTimerB: ReturnType<typeof setTimeout> | null = null;
    let elapsedTimer: ReturnType<typeof setInterval> | null = null;

    let isBusy = $derived(stage === 'uploading' || stage === 'processing' || stage === 'compiling');
    let stageIndex = $derived(STAGES.findIndex((step) => step.id === stage));
    let galleryItems = $derived(summary?.items ?? []);
    let passRate = $derived(
        summary && summary.total_trays > 0 ? (summary.accepted / summary.total_trays) * 100 : 0
    );

    function clearTimers() {
        if (stageTimerA) {
            clearTimeout(stageTimerA);
            stageTimerA = null;
        }
        if (stageTimerB) {
            clearTimeout(stageTimerB);
            stageTimerB = null;
        }
        if (elapsedTimer) {
            clearInterval(elapsedTimer);
            elapsedTimer = null;
        }
    }

    onDestroy(clearTimers);

    function openPicker() {
        if (isBusy) return;
        fileInput?.click();
    }

    function onDragOver(event: DragEvent) {
        event.preventDefault();
        if (!isBusy) isDragging = true;
    }

    function onDragLeave() {
        isDragging = false;
    }

    function onDrop(event: DragEvent) {
        event.preventDefault();
        isDragging = false;
        if (isBusy) return;
        const file = event.dataTransfer?.files?.[0];
        if (file) handleFile(file);
    }

    function onFileSelected(event: Event) {
        const input = event.currentTarget as HTMLInputElement;
        const file = input.files?.[0];
        input.value = '';
        if (file) handleFile(file);
    }

    function handleFile(file: File) {
        if (!file.name.toLowerCase().endsWith('.zip')) {
            stage = 'error';
            errorMessage = `Invalid file "${file.name}". Only .zip archives are accepted.`;
            return;
        }
        if (file.size > MAX_ZIP_BYTES) {
            stage = 'error';
            errorMessage = `ZIP archive is ${formatBytes(file.size)} — the maximum upload size is 15 MB.`;
            return;
        }
        void uploadBatch(file);
    }

    async function uploadBatch(file: File) {
        clearTimers();
        summary = null;
        errorMessage = '';
        noticeMessage = '';
        failedImages = {};
        selectedFileName = file.name;
        uploadBytes = file.size;
        elapsedMs = 0;
        stage = 'uploading';

        const startedAt = performance.now();
        elapsedTimer = setInterval(() => {
            elapsedMs = performance.now() - startedAt;
        }, 100);
        stageTimerA = setTimeout(() => {
            if (stage === 'uploading') stage = 'processing';
        }, 550);
        stageTimerB = setTimeout(() => {
            if (stage === 'uploading' || stage === 'processing') stage = 'compiling';
        }, 1800);

        try {
            const response = await fetch(BATCH_ENDPOINT, {
                method: 'POST',
                headers: { 'Content-Type': 'application/zip' },
                body: file
            });

            const payload = (await response.json().catch(() => null)) as BatchSummary | null;
            if (!response.ok || !payload) {
                throw new Error(payload?.error ?? `Server returned ${response.status}`);
            }

            clearTimers();
            summary = payload;
            elapsedMs = payload.processing_time_ms;
            stage = 'done';
            noticeMessage = `Batch ${payload.batch_id} completed: ${payload.accepted} accepted / ${payload.rejected} rejected.`;
        } catch (err) {
            clearTimers();
            stage = 'error';
            errorMessage = err instanceof Error ? err.message : 'Batch ZIP upload failed.';
        }
    }

    function resetBatch() {
        clearTimers();
        stage = 'idle';
        summary = null;
        errorMessage = '';
        noticeMessage = '';
        selectedFileName = '';
        uploadBytes = 0;
        elapsedMs = 0;
        failedImages = {};
    }

    async function downloadMasterReport() {
        if (!summary || isDownloadingPdf) return;
        isDownloadingPdf = true;
        noticeMessage = 'Preparing master report download...';
        try {
            const response = await fetch(summary.pdf_download_url);
            if (!response.ok) {
                throw new Error(`Report download failed (${response.status})`);
            }

            // The backend serves the LaTeX source when pdflatex is unavailable;
            // X-Report-Format tells us which artifact actually arrived.
            const servedFormat = response.headers.get('X-Report-Format') === 'tex' ? 'tex' : 'pdf';
            const blob = await response.blob();
            const blobUrl = window.URL.createObjectURL(blob);
            const anchor = document.createElement('a');
            anchor.href = blobUrl;
            anchor.download = `BawalVision-Batch-${summary.batch_id}.${servedFormat}`;
            document.body.appendChild(anchor);
            anchor.click();
            document.body.removeChild(anchor);
            window.URL.revokeObjectURL(blobUrl);

            noticeMessage = servedFormat === 'pdf'
                ? `Master PDF report for ${summary.batch_id} downloaded.`
                : `pdflatex unavailable on server — LaTeX master source for ${summary.batch_id} downloaded.`;
        } catch (err) {
            noticeMessage = '';
            errorMessage = err instanceof Error ? err.message : 'Failed to download master report.';
        } finally {
            isDownloadingPdf = false;
        }
    }

    function markImageFailed(url: string) {
        failedImages = { ...failedImages, [url]: true };
    }

    function formatBytes(bytes: number): string {
        if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
        if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
        return `${bytes} B`;
    }

    function formatMs(ms: number): string {
        return ms >= 1000 ? `${(ms / 1000).toFixed(2)} s` : `${ms.toFixed(1)} ms`;
    }

    function formatPercent(value: number): string {
        return `${(value * 100).toFixed(0)}%`;
    }
</script>

<div class="batch-view">
    <header class="batch-header card">
        <div class="batch-heading">
            <span class="batch-kicker">HIGH-THROUGHPUT PARALLEL INSPECTION</span>
            <h2>Batch Inspection</h2>
            <p>
                Upload a ZIP archive of food tray photos. The C++ backend extracts every image,
                runs YOLOv8s INT8 inference across OpenMP worker threads, and compiles a
                multi-page LaTeX master report per batch.
            </p>
        </div>
        <div class="batch-badge">
            <span class="badge-dot"></span>
            OpenMP + AVX2
        </div>
    </header>

    <section class="upload-card card">
        <button
            type="button"
            class="dropzone"
            class:dragging={isDragging}
            class:busy={isBusy}
            aria-label="Upload batch ZIP archive"
            disabled={isBusy}
            onclick={openPicker}
            ondragover={onDragOver}
            ondragleave={onDragLeave}
            ondrop={onDrop}
        >
            <span class="dropzone-icon">📦</span>
            <span class="dropzone-title">
                {isBusy ? 'Batch in progress...' : 'Drop batch ZIP here or click to browse'}
            </span>
            <span class="dropzone-hint">.zip archive · max 15 MB · up to 30 tray images</span>
            {#if selectedFileName}
                <span class="dropzone-file">
                    {selectedFileName}
                    {#if uploadBytes > 0}· {formatBytes(uploadBytes)}{/if}
                </span>
            {/if}
        </button>
        <input
            type="file"
            accept=".zip,application/zip"
            class="hidden-file-input"
            aria-label="Batch ZIP file input"
            bind:this={fileInput}
            onchange={onFileSelected}
        />

        <div class="stage-track" aria-live="polite">
            {#each STAGES as step, index}
                {@const state = stage === 'done'
                    ? 'complete'
                    : (isBusy
                        ? (index < stageIndex ? 'complete' : (index === stageIndex ? 'active' : 'pending'))
                        : (stage === 'error' && index === stageIndex ? 'failed' : 'pending'))}
                <div class="stage-step {state}">
                    <span class="stage-node"></span>
                    <span class="stage-label">{step.label}</span>
                </div>
            {/each}
        </div>

        {#if isBusy}
            <div class="progress-line">
                <div class="progress-bar"></div>
                <span class="progress-text">Elapsed {formatMs(elapsedMs)}</span>
            </div>
        {/if}

        {#if errorMessage}
            <div class="banner error" role="alert">{errorMessage}</div>
        {/if}
        {#if noticeMessage}
            <div class="banner notice" role="status">{noticeMessage}</div>
        {/if}
    </section>

    {#if summary}
        <section class="kpi-panel card">
            <div class="kpi-grid">
                <div class="kpi-card wide">
                    <span class="kpi-label">Batch ID</span>
                    <strong class="kpi-value mono">{summary.batch_id}</strong>
                </div>
                <div class="kpi-card">
                    <span class="kpi-label">Parallel Compute Time</span>
                    <strong class="kpi-value">{formatMs(summary.processing_time_ms)}</strong>
                    <span class="kpi-sub">OpenMP SIMD multi-core</span>
                </div>
                <div class="kpi-card">
                    <span class="kpi-label">Total Trays</span>
                    <strong class="kpi-value">{summary.total_trays}</strong>
                    <span class="kpi-sub">{galleryItems.length} images extracted</span>
                </div>
                <div class="kpi-card accepted">
                    <span class="kpi-label">Accepted</span>
                    <strong class="kpi-value">{summary.accepted}</strong>
                    <span class="kpi-sub">{passRate.toFixed(1)}% pass rate</span>
                </div>
                <div class="kpi-card rejected">
                    <span class="kpi-label">Rejected</span>
                    <strong class="kpi-value">{summary.rejected}</strong>
                    <span class="kpi-sub">spoilage flagged</span>
                </div>
            </div>

            <div class="kpi-actions">
                <button
                    type="button"
                    class="btn primary"
                    disabled={isDownloadingPdf}
                    onclick={downloadMasterReport}
                >
                    {isDownloadingPdf ? '⏳ Preparing report...' : '📄 Download Master PDF Report'}
                </button>
                <button type="button" class="btn ghost" disabled={isBusy} onclick={resetBatch}>
                    ↺ Process another batch
                </button>
            </div>
        </section>

        <section class="gallery-section">
            <div class="gallery-header">
                <h3>Extracted Tray Gallery</h3>
                <span class="gallery-count">{galleryItems.length} trays</span>
            </div>

            {#if galleryItems.length === 0}
                <div class="gallery-empty card">No tray images were extracted from this archive.</div>
            {:else}
                <div class="tray-grid">
                    {#each galleryItems as item (item.index + '-' + item.filename)}
                        <article class="tray-card">
                            <div class="tray-thumb">
                                {#if item.annotated_url && !failedImages[item.annotated_url]}
                                    <img
                                        src={item.annotated_url}
                                        alt={`Annotated tray ${item.filename}`}
                                        loading="lazy"
                                        onerror={() => markImageFailed(item.annotated_url)}
                                    />
                                {:else}
                                    <div class="thumb-fallback">
                                        <span class="fallback-icon">🗂️</span>
                                        <span>Annotated frame unavailable</span>
                                    </div>
                                {/if}
                                <span class="verdict-badge" class:rejected={!item.accepted}>
                                    {item.accepted ? 'ACCEPTED' : 'REJECTED'}
                                </span>
                            </div>

                            <div class="tray-body">
                                <div class="tray-title-row">
                                    <strong class="tray-name" title={item.filename}>{item.filename}</strong>
                                    <span class="tray-index">Tray #{item.index}</span>
                                </div>

                                <div class="tray-metrics">
                                    <span class="metric-chip">🌡️ {item.surface_temp.toFixed(1)} °C</span>
                                    <span class="metric-chip">⏳ t_safe {item.t_safe_hours.toFixed(1)} h</span>
                                    <span class="metric-chip">{item.detections.length} detections</span>
                                </div>

                                <div class="detection-log">
                                    {#if item.detections.length === 0}
                                        <span class="no-detection">No food item detected in this tray.</span>
                                    {:else}
                                        {#each item.detections as detection}
                                            <div class="detection-line">
                                                <span class="class-name">{detection.class_name}</span>
                                                <span class="det-stat">conf {formatPercent(detection.confidence)}</span>
                                                <span class="det-stat">fresh {formatPercent(detection.freshness)}</span>
                                            </div>
                                        {/each}
                                    {/if}
                                </div>
                            </div>
                        </article>
                    {/each}
                </div>
            {/if}
        </section>
    {/if}
</div>

<style>
    .batch-view {
        display: flex;
        flex-direction: column;
        gap: 16px;
        width: 100%;
    }

    .batch-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 16px;
        padding: 16px 18px;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        box-shadow: var(--shadow-card);
    }

    .batch-heading h2 {
        margin: 2px 0 4px;
        font-size: 1.05rem;
        color: var(--text-primary);
    }

    .batch-heading p {
        margin: 0;
        max-width: 680px;
        font-size: 0.76rem;
        line-height: 1.5;
        color: var(--text-muted);
    }

    .batch-kicker {
        font-size: 0.62rem;
        font-weight: 800;
        letter-spacing: 0.08em;
        color: var(--accent-blue);
    }

    .batch-badge {
        display: inline-flex;
        align-items: center;
        gap: 7px;
        flex-shrink: 0;
        font-size: 0.66rem;
        font-weight: 700;
        color: var(--accent-purple);
        background: var(--accent-purple-dim);
        border: 1px solid #ddd0fb;
        border-radius: var(--radius-pill);
        padding: 6px 11px;
    }

    .badge-dot {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        background: var(--accent-purple);
        box-shadow: 0 0 6px var(--accent-purple);
    }

    .upload-card {
        display: flex;
        flex-direction: column;
        gap: 14px;
        padding: 16px;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        box-shadow: var(--shadow-card);
    }

    .dropzone {
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        gap: 6px;
        width: 100%;
        padding: 28px 18px;
        background: var(--bg-inset);
        border: 2px dashed var(--border-highlight);
        border-radius: var(--radius-md);
        color: var(--text-secondary);
        cursor: pointer;
        transition: border-color 0.16s ease, background 0.16s ease, transform 0.12s ease;
    }

    .dropzone:hover:not(:disabled),
    .dropzone.dragging {
        border-color: var(--accent-blue);
        background: var(--accent-blue-dim);
    }

    .dropzone:active:not(:disabled) {
        transform: scale(0.995);
    }

    .dropzone.busy {
        cursor: progress;
        opacity: 0.85;
    }

    .dropzone-icon {
        font-size: 1.8rem;
        line-height: 1;
    }

    .dropzone-title {
        font-size: 0.84rem;
        font-weight: 700;
        color: var(--text-primary);
    }

    .dropzone-hint {
        font-size: 0.66rem;
        color: var(--text-muted);
        font-family: var(--font-mono);
    }

    .dropzone-file {
        margin-top: 4px;
        font-size: 0.68rem;
        font-weight: 700;
        font-family: var(--font-mono);
        color: var(--accent-blue);
        background: var(--accent-blue-dim);
        border: 1px solid #c7dfef;
        border-radius: var(--radius-pill);
        padding: 3px 10px;
    }

    .hidden-file-input {
        display: none;
    }

    .stage-track {
        display: grid;
        grid-template-columns: repeat(3, 1fr);
        gap: 8px;
    }

    .stage-step {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 8px 10px;
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
        background: var(--bg-inset);
    }

    .stage-node {
        width: 10px;
        height: 10px;
        border-radius: 50%;
        background: var(--border-highlight);
        flex-shrink: 0;
    }

    .stage-label {
        font-size: 0.64rem;
        font-weight: 700;
        color: var(--text-muted);
        line-height: 1.25;
    }

    .stage-step.active {
        border-color: var(--accent-blue-light);
        background: var(--accent-blue-dim);
    }

    .stage-step.active .stage-node {
        background: var(--accent-blue);
        box-shadow: 0 0 0 3px rgba(54, 121, 168, 0.2);
        animation: nodePulse 1s ease-in-out infinite;
    }

    .stage-step.active .stage-label {
        color: var(--accent-blue);
    }

    .stage-step.complete {
        border-color: #bbf7d0;
        background: var(--accent-green-dim);
    }

    .stage-step.complete .stage-node {
        background: var(--accent-green);
    }

    .stage-step.complete .stage-label {
        color: #15803d;
    }

    .stage-step.failed {
        border-color: #fecaca;
        background: var(--accent-red-dim);
    }

    .stage-step.failed .stage-node {
        background: var(--accent-red);
    }

    .stage-step.failed .stage-label {
        color: var(--accent-red);
    }

    @keyframes nodePulse {
        0%, 100% { transform: scale(1); }
        50% { transform: scale(1.25); }
    }

    .progress-line {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .progress-bar {
        position: relative;
        flex: 1;
        height: 5px;
        border-radius: var(--radius-pill);
        background: var(--bg-chip);
        overflow: hidden;
    }

    .progress-bar::after {
        content: '';
        position: absolute;
        inset: 0;
        width: 40%;
        border-radius: var(--radius-pill);
        background: linear-gradient(90deg, var(--accent-blue), var(--accent-blue-light));
        animation: indeterminate 1.1s ease-in-out infinite;
    }

    @keyframes indeterminate {
        0% { transform: translateX(-100%); }
        100% { transform: translateX(250%); }
    }

    .progress-text {
        font-size: 0.66rem;
        font-family: var(--font-mono);
        color: var(--text-muted);
        white-space: nowrap;
    }

    .banner {
        padding: 9px 12px;
        border-radius: var(--radius-sm);
        font-size: 0.72rem;
        font-weight: 600;
        line-height: 1.4;
    }

    .banner.error {
        background: var(--accent-red-dim);
        border: 1px solid #fecaca;
        color: #b91c1c;
    }

    .banner.notice {
        background: var(--accent-green-dim);
        border: 1px solid #bbf7d0;
        color: #15803d;
    }

    .kpi-panel {
        display: flex;
        flex-direction: column;
        gap: 14px;
        padding: 16px;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
        box-shadow: var(--shadow-card);
    }

    .kpi-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
        gap: 10px;
    }

    .kpi-card {
        display: flex;
        flex-direction: column;
        gap: 3px;
        padding: 10px 12px;
        background: var(--bg-inset);
        border: 1px solid var(--border-subtle);
        border-radius: var(--radius-sm);
    }

    .kpi-card.wide {
        grid-column: span 2;
    }

    .kpi-card.accepted {
        border-color: #bbf7d0;
        background: var(--accent-green-dim);
    }

    .kpi-card.rejected {
        border-color: #fecaca;
        background: var(--accent-red-dim);
    }

    .kpi-label {
        font-size: 0.6rem;
        font-weight: 700;
        letter-spacing: 0.05em;
        text-transform: uppercase;
        color: var(--text-muted);
    }

    .kpi-value {
        font-size: 1.02rem;
        font-weight: 800;
        color: var(--text-primary);
        overflow-wrap: anywhere;
    }

    .kpi-value.mono {
        font-family: var(--font-mono);
        font-size: 0.86rem;
    }

    .kpi-card.accepted .kpi-value {
        color: #15803d;
    }

    .kpi-card.rejected .kpi-value {
        color: var(--accent-red);
    }

    .kpi-sub {
        font-size: 0.62rem;
        color: var(--text-muted);
    }

    .kpi-actions {
        display: flex;
        gap: 8px;
        flex-wrap: wrap;
    }

    .btn {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        font-size: 0.72rem;
        font-weight: 700;
        padding: 8px 14px;
        border-radius: var(--radius-sm);
        border: 1px solid transparent;
        cursor: pointer;
        transition: background 0.14s ease, border-color 0.14s ease, color 0.14s ease;
    }

    .btn:disabled {
        opacity: 0.6;
        cursor: not-allowed;
    }

    .btn.primary {
        background: var(--accent-blue);
        border-color: var(--accent-blue);
        color: var(--text-white);
    }

    .btn.primary:hover:not(:disabled) {
        background: var(--accent-blue-light);
        border-color: var(--accent-blue-light);
    }

    .btn.ghost {
        background: transparent;
        border-color: var(--border-subtle);
        color: var(--text-secondary);
    }

    .btn.ghost:hover:not(:disabled) {
        background: var(--bg-chip);
        color: var(--text-primary);
    }

    .gallery-section {
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .gallery-header {
        display: flex;
        align-items: baseline;
        justify-content: space-between;
        gap: 10px;
    }

    .gallery-header h3 {
        margin: 0;
        font-size: 0.86rem;
        color: var(--text-primary);
    }

    .gallery-count {
        font-size: 0.66rem;
        font-weight: 700;
        font-family: var(--font-mono);
        color: var(--accent-blue);
        background: var(--accent-blue-dim);
        border: 1px solid #c7dfef;
        border-radius: var(--radius-pill);
        padding: 2px 9px;
    }

    .gallery-empty {
        padding: 22px;
        text-align: center;
        font-size: 0.76rem;
        color: var(--text-muted);
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-lg);
    }

    .tray-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
        gap: 12px;
    }

    .tray-card {
        display: flex;
        flex-direction: column;
        background: var(--bg-card);
        border: 1px solid var(--border-card);
        border-radius: var(--radius-md);
        overflow: hidden;
        box-shadow: var(--shadow-card);
    }

    .tray-thumb {
        position: relative;
        aspect-ratio: 4 / 3;
        background: var(--bg-inset);
        border-bottom: 1px solid var(--border-subtle);
    }

    .tray-thumb img {
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
        padding: 3px 8px;
        border-radius: var(--radius-pill);
        background: var(--accent-green);
        color: var(--text-white);
        box-shadow: var(--shadow-sm);
    }

    .verdict-badge.rejected {
        background: var(--accent-red);
    }

    .tray-body {
        display: flex;
        flex-direction: column;
        gap: 8px;
        padding: 10px;
    }

    .tray-title-row {
        display: flex;
        align-items: baseline;
        justify-content: space-between;
        gap: 8px;
    }

    .tray-name {
        font-size: 0.76rem;
        color: var(--text-primary);
        font-family: var(--font-mono);
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .tray-index {
        font-size: 0.62rem;
        font-weight: 700;
        color: var(--text-muted);
        flex-shrink: 0;
    }

    .tray-metrics {
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

    .no-detection {
        font-size: 0.62rem;
        color: var(--text-muted);
        font-style: italic;
    }

    @media (max-width: 768px) {
        .batch-header {
            flex-direction: column;
            align-items: flex-start;
        }

        .stage-track {
            grid-template-columns: 1fr;
        }

        .kpi-card.wide {
            grid-column: span 1;
        }

        .tray-grid {
            grid-template-columns: 1fr;
        }
    }
</style>
