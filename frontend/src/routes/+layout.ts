// Hybrid Rendering / Islands Architecture Configuration:
// Server renders heavy initial layouts, shells, and structural DOM (SSR)
// Client hydrates and runs lightweight interactive animations & streams (CSR)
export const ssr = true;
export const prerender = false;

