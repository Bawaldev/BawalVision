<script lang="ts">
    interface Props {
        grafanaHost?: string;
        panelId?: number | null;
        height?: string;
        theme?: 'dark' | 'light';
        kiosk?: boolean;
    }

    let {
        grafanaHost = 'http://localhost:3002',
        panelId = null,
        height = '480px',
        theme = 'dark',
        kiosk = true
    }: Props = $props();

    let isIframeLoaded = $state(false);
    let hasLoadError = $state(false);

    // Compute Grafana Embed URL
    let embedUrl = $derived.by(() => {
        const base = grafanaHost.replace(/\/+$/, '');
        const themeParam = `theme=${theme}`;
        const kioskParam = kiosk ? '&kiosk' : '';

        if (panelId !== null && panelId !== undefined) {
            // Embed single panel
            return `${base}/d-solo/bawalvision_analytics/_?orgId=1&panelId=${panelId}&${themeParam}${kioskParam}`;
        }
        // Embed complete dashboard
        return `${base}/d/bawalvision_analytics/_?orgId=1&${themeParam}${kioskParam}`;
    });

    function handleIframeLoad() {
        isIframeLoaded = true;
        hasLoadError = false;
    }

    function openFullGrafana() {
        window.open(`${grafanaHost}/d/bawalvision_analytics/_`, '_blank');
    }
</script>

<div class="grafana-embed-card">
    <div class="embed-header">
        <div class="header-left">
            <div class="grafana-badge">
                <svg viewBox="0 0 24 24" fill="currentColor" class="grafana-icon">
                    <path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"/>
                </svg>
                <span>AI Model Real-Time Monitor</span>
            </div>
            <span class="live-pill">YOLOv8 & CV Engine</span>
        </div>
        <div class="header-right">
            <button class="action-btn" onclick={openFullGrafana} title="Buka Dashboard Grafana di Tab Baru">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="btn-icon">
                    <path stroke-linecap="round" stroke-linejoin="round" d="M10 6H6a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2v-4M14 4h6m0 0v6m0-6L10 14"/>
                </svg>
                <span>Buka Fullscreen (:3002)</span>
            </button>
        </div>
    </div>

    <div class="embed-canvas" style="height: {height};">
        <iframe
            src={embedUrl}
            title="BawalVision Real-Time AI Model Monitoring"
            width="100%"
            height="100%"
            frameborder="0"
            onload={handleIframeLoad}
            onerror={() => hasLoadError = true}
        ></iframe>

        {#if !isIframeLoaded}
            <div class="loading-overlay">
                <div class="spinner"></div>
                <p>Memuat telemetri model AI dari Grafana (:3002)...</p>
                <span class="sub-hint">Pastikan container Grafana sudah aktif via <code>just compose-up grafana</code></span>
            </div>
        {/if}
    </div>
</div>

<style>
    .grafana-embed-card {
        background: #111827;
        border: 1px solid #1F2937;
        border-radius: 12px;
        overflow: hidden;
        display: flex;
        flex-direction: column;
        margin-top: 0.5rem;
        box-shadow: 0 4px 16px rgba(0, 0, 0, 0.4);
    }

    .embed-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 0.6rem 1rem;
        background: #1F2937;
        border-bottom: 1px solid #374151;
    }

    .header-left {
        display: flex;
        align-items: center;
        gap: 0.6rem;
    }

    .grafana-badge {
        display: inline-flex;
        align-items: center;
        gap: 0.4rem;
        font-size: 0.82rem;
        font-weight: 600;
        color: #10B981;
    }

    .grafana-icon {
        width: 18px;
        height: 18px;
        color: #F59E0B;
    }

    .live-pill {
        background: rgba(16, 185, 129, 0.15);
        color: #10B981;
        font-size: 0.7rem;
        padding: 0.15rem 0.5rem;
        border-radius: 9999px;
        font-weight: 500;
        border: 1px solid rgba(16, 185, 129, 0.3);
    }

    .action-btn {
        display: inline-flex;
        align-items: center;
        gap: 0.4rem;
        background: #374151;
        color: #E5E7EB;
        border: 1px solid #4B5563;
        padding: 0.35rem 0.75rem;
        border-radius: 6px;
        font-size: 0.75rem;
        cursor: pointer;
        transition: all 0.2s ease;
    }

    .action-btn:hover {
        background: #4B5563;
        color: #FFFFFF;
        border-color: #6B7280;
    }

    .btn-icon {
        width: 14px;
        height: 14px;
    }

    .embed-canvas {
        position: relative;
        width: 100%;
        background: #0B0F19;
    }

    iframe {
        display: block;
        width: 100%;
        height: 100%;
        border: none;
    }

    .loading-overlay {
        position: absolute;
        inset: 0;
        background: rgba(11, 15, 25, 0.85);
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        gap: 0.5rem;
        color: #9CA3AF;
        font-size: 0.85rem;
        z-index: 10;
    }

    .spinner {
        width: 28px;
        height: 28px;
        border: 3px solid rgba(16, 185, 129, 0.2);
        border-top-color: #10B981;
        border-radius: 50%;
        animation: spin 0.8s linear infinite;
    }

    .sub-hint {
        font-size: 0.72rem;
        color: #6B7280;
    }

    .sub-hint code {
        background: #1F2937;
        color: #10B981;
        padding: 0.1rem 0.3rem;
        border-radius: 4px;
    }

    @keyframes spin {
        to { transform: rotate(360deg); }
    }
</style>
