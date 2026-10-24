<script lang="ts">
    import GrafanaAnalyticsEmbed from '../components/GrafanaAnalyticsEmbed.svelte';

    const grafanaUrl = 'http://localhost:3002';
</script>

<div class="grafana-view">
    <header class="grafana-view-header card">
        <div>
            <span class="grafana-kicker">MONITORING</span>
            <h2>Grafana Dashboard</h2>
            <p>
                Embedded analytics workspace for throughput, spoilage trends, and
                inference telemetry served from <code>{grafanaUrl}</code>.
            </p>
        </div>
        <a
            class="grafana-open-btn"
            href={grafanaUrl}
            target="_blank"
            rel="noopener noreferrer"
        >
            Open Grafana in New Tab
        </a>
    </header>

    <GrafanaAnalyticsEmbed height="calc(100vh - 280px)" />
</div>

<style>
    .grafana-view {
        display: flex;
        flex-direction: column;
        gap: 16px;
        width: 100%;
    }

    .grafana-view-header {
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

    .grafana-view-header h2 {
        margin: 2px 0 4px;
        font-size: 1.05rem;
        color: var(--text-primary);
    }

    .grafana-view-header p {
        margin: 0;
        max-width: 680px;
        font-size: 0.76rem;
        line-height: 1.5;
        color: var(--text-muted);
    }

    .grafana-view-header code {
        font-family: var(--font-mono);
        font-size: 0.72rem;
    }

    .grafana-kicker {
        font-size: 0.62rem;
        font-weight: 800;
        letter-spacing: 0.08em;
        color: var(--accent-orange, #e67e22);
    }

    .grafana-open-btn {
        flex-shrink: 0;
        display: inline-flex;
        align-items: center;
        justify-content: center;
        padding: 8px 14px;
        border-radius: var(--radius-pill);
        background: var(--accent-blue);
        border: 1px solid var(--accent-blue);
        color: var(--text-white);
        font-size: 0.72rem;
        font-weight: 700;
        text-decoration: none;
        transition: background 0.15s ease;
    }

    .grafana-open-btn:hover {
        background: var(--accent-blue-light);
    }

    @media (max-width: 768px) {
        .grafana-view-header {
            flex-direction: column;
            align-items: flex-start;
        }
    }
</style>
