<script lang="ts">
    import favicon from '$lib/assets/favicon.svg';
    import '../app.css';

    let { children } = $props();
</script>

<svelte:head>
    <link rel="icon" href={favicon} />
    <meta name="theme-color" content="#07111f" />
</svelte:head>

{@render children()}
