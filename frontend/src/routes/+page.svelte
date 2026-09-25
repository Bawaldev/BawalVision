<script lang="ts">
    let activeMenu = $state('Dashboard');

    const menus = [
        { label: 'Dashboard', icon: '▦' },
        { label: 'Monitoring', icon: '◉' },
        { label: 'Machine Camera', icon: '▣' },
        { label: 'Results', icon: '▤' },
        { label: 'Settings', icon: '⚙' }
    ];
</script>

<svelte:head>
    <title>BawalVision | {activeMenu}</title>
    <meta
        name="description"
        content="BawalVision food inspection and machine vision dashboard"
    />
</svelte:head>

<div class="app-shell">
    <aside class="sidebar">
        <div class="brand">
            <div class="brand-mark">B</div>
            <div>
                <strong>Bawal<span>Vision</span></strong>
                <small>AI INSPECTION SYSTEM</small>
            </div>
        </div>

        <nav>
            <p class="nav-label">MAIN MENU</p>

            {#each menus as menu}
                <button
                    class:active={activeMenu === menu.label}
                    class="nav-item"
                    onclick={() => (activeMenu = menu.label)}
                >
                    <span class="nav-icon">{menu.icon}</span>
                    {menu.label}
                    {#if menu.label === 'Monitoring'}
                        <span class="live-dot"></span>
                    {/if}
                </button>
            {/each}
        </nav>

        <div class="sidebar-footer">
            <div class="system-status">
                <span></span>
                <div>
                    <strong>System online</strong>
                    <small>All services operational</small>
                </div>
            </div>
            <div class="profile">
                <div class="avatar">AD</div>
                <div>
                    <strong>Admin User</strong>
                    <small>Administrator</small>
                </div>
                <span class="more">•••</span>
            </div>
        </div>
    </aside>

    <main class="content">
        <header class="topbar">
            <div>
                <p class="eyebrow">FOOD QUALITY CONTROL</p>
                <h1>{activeMenu}</h1>
            </div>

            <div class="top-actions">
                <div class="connection">
                    <span></span>
                    Live connection
                </div>
                <button class="icon-button" aria-label="Notifications">♢</button>
                <div class="date">September 25, 2026</div>
            </div>
        </header>

        {#if activeMenu === 'Dashboard'}
            <section class="welcome">
                <div>
                    <p class="eyebrow">GOOD MORNING, ADMIN</p>
                    <h2>Inspection overview</h2>
                    <p class="muted">Monitor your production line and food quality in real time.</p>
                </div>
                <button class="primary-button" onclick={() => (activeMenu = 'Machine Camera')}>
                    <span>＋</span> Start inspection
                </button>
            </section>

            <section class="stats-grid">
                <div class="stat-card">
                    <div class="stat-icon blue">◉</div>
                    <div><span>Total inspections</span><strong>12,840</strong><small class="positive">↑ 12.5% this month</small></div>
                </div>
                <div class="stat-card">
                    <div class="stat-icon green">✓</div>
                    <div><span>Passed items</span><strong>11,967</strong><small class="positive">↑ 8.2% this month</small></div>
                </div>
                <div class="stat-card">
                    <div class="stat-icon orange">!</div>
                    <div><span>Rejected items</span><strong>873</strong><small class="negative">↓ 3.1% this month</small></div>
                </div>
                <div class="stat-card">
                    <div class="stat-icon purple">⌁</div>
                    <div><span>Accuracy rate</span><strong>98.4%</strong><small class="positive">↑ 1.8% this month</small></div>
                </div>
            </section>

            <section class="dashboard-grid">
                <div class="panel chart-panel">
                    <div class="panel-heading">
                        <div><h3>Inspection activity</h3><p>Daily inspection performance</p></div>
                        <select aria-label="Period"><option>Last 7 days</option><option>Last 30 days</option></select>
                    </div>
                    <div class="chart">
                        <div class="chart-lines"><i></i><i></i><i></i><i></i><i></i></div>
                        <div class="bars">
                            {#each [46, 61, 52, 74, 63, 86, 78] as height, i}
                                <div class="bar-wrap"><div class="bar" style={`height:${height}%`}></div><small>{['Mon','Tue','Wed','Thu','Fri','Sat','Sun'][i]}</small></div>
                            {/each}
                        </div>
                    </div>
                </div>

                <div class="panel">
                    <div class="panel-heading"><div><h3>Food categories</h3><p>Inspections by category</p></div><button class="text-button">View all →</button></div>
                    <div class="category-list">
                        {#each [['Ayam','3,240','91%','blue'],['Bayam','2,850','86%','green'],['Jeruk','2,410','79%','orange'],['Tofu','1,920','73%','purple']] as item}
                            <div class="category"><div class={`category-dot ${item[3]}`}></div><div class="category-info"><strong>{item[0]}</strong><small>{item[1]} inspections</small></div><b>{item[2]}</b></div>
                        {/each}
                    </div>
                </div>
            </section>

            <section class="panel recent">
                <div class="panel-heading"><div><h3>Recent inspections</h3><p>Latest machine inspection results</p></div><button class="text-button" onclick={() => (activeMenu = 'Results')}>View all results →</button></div>
                <div class="table-wrap">
                    <table>
                        <thead><tr><th>INSPECTION ID</th><th>PRODUCT</th><th>CAMERA</th><th>DATE & TIME</th><th>RESULT</th></tr></thead>
                        <tbody>
                            <tr><td>#INS-009842</td><td><strong>Fresh Bayam</strong></td><td>Camera 01</td><td>Today, 10:42 AM</td><td><span class="badge passed">Passed</span></td></tr>
                            <tr><td>#INS-009841</td><td><strong>Organic Tofu</strong></td><td>Camera 02</td><td>Today, 10:39 AM</td><td><span class="badge review">Review</span></td></tr>
                            <tr><td>#INS-009840</td><td><strong>Chicken Breast</strong></td><td>Camera 01</td><td>Today, 10:35 AM</td><td><span class="badge passed">Passed</span></td></tr>
                        </tbody>
                    </table>
                </div>
            </section>
        {:else}
            <section class="empty-page panel">
                <div class="empty-icon">{menus.find((menu) => menu.label === activeMenu)?.icon}</div>
                <h2>{activeMenu}</h2>
                <p class="muted">This module is ready to connect to the BawalVision backend.</p>
                <button class="primary-button">Configure module</button>
            </section>
        {/if}
    </main>
</div>
