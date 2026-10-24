<script lang="ts">
    import InspectionAuditPanel from '../components/InspectionAuditPanel.svelte';
</script>

<div class="reports-view">
    <header class="reports-header card">
        <div class="reports-heading">
            <span class="reports-kicker">AUDIT &amp; COMPLIANCE</span>
            <h2>Inspection Reports</h2>
            <p>
                Rolling 50-slot evidence buffer with annotated bounding-box snapshots.
                Export individual trays or the whole batch as Markdown.
            </p>
        </div>
        <div class="reports-badge">
            <span class="badge-dot"></span>
            Annotated evidence
        </div>
    </header>

    <InspectionAuditPanel />
</div>

<style>
    .reports-view {
        display: flex;
        flex-direction: column;
        gap: 16px;
        width: 100%;
    }

    .reports-header {
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

    .reports-heading h2 {
        margin: 2px 0 4px;
        font-size: 1.05rem;
        color: var(--text-primary);
    }

    .reports-heading p {
        margin: 0;
        max-width: 640px;
        font-size: 0.76rem;
        line-height: 1.5;
        color: var(--text-muted);
    }

    .reports-kicker {
        font-size: 0.62rem;
        font-weight: 800;
        letter-spacing: 0.08em;
        color: var(--accent-blue);
    }

    .reports-badge {
        display: inline-flex;
        align-items: center;
        gap: 7px;
        flex-shrink: 0;
        font-size: 0.66rem;
        font-weight: 700;
        color: var(--accent-green);
        background: #ecfdf5;
        border: 1px solid #bbf7d0;
        border-radius: var(--radius-pill);
        padding: 6px 11px;
    }

    .badge-dot {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        background: var(--accent-green);
        box-shadow: 0 0 6px var(--accent-green);
    }

    @media (max-width: 768px) {
        .reports-header {
            flex-direction: column;
            align-items: flex-start;
        }
    }
</style>
