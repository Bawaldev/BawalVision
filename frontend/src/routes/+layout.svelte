<script lang="ts">
    import { afterNavigate } from '$app/navigation';
    import { env } from '$env/dynamic/public';
    import { onMount } from 'svelte';
    import favicon from '$lib/assets/favicon.svg';
    import '../app.css';

    let { children } = $props();

    // GA4 measurement ID (G-XXXXXXX) from PUBLIC_GA_MEASUREMENT_ID; disabled when unset.
    const GA_ID = env.PUBLIC_GA_MEASUREMENT_ID;
    let gtag: ((...args: unknown[]) => void) | null = null;

    onMount(() => {
        if (!GA_ID || gtag) return;
        // gtag queues into dataLayer, so it works before the async script loads.
        const dataLayer = ((window as any).dataLayer = (window as any).dataLayer ?? []);
        gtag = (...args: unknown[]) => dataLayer.push(args);
        gtag('js', new Date());
        gtag('config', GA_ID);
        const script = document.createElement('script');
        script.async = true;
        script.src = `https://www.googletagmanager.com/gtag/js?id=${encodeURIComponent(GA_ID)}`;
        document.head.appendChild(script);
    });

    // The config call counts the first page view; SPA navigations need their own events.
    let firstNavigation = true;
    afterNavigate(({ to }) => {
        if (firstNavigation) {
            firstNavigation = false;
            return;
        }
        if (to && gtag) gtag('event', 'page_view', { page_path: to.url.pathname + to.url.search });
    });
</script>

<svelte:head>
    <link rel="icon" href={favicon} />
    <meta name="theme-color" content="#07111f" />
</svelte:head>

{@render children()}
