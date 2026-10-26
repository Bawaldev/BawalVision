import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig, loadEnv } from 'vite';

export default defineConfig(({ mode }) => {
	const env = loadEnv(mode, (globalThis as any).process?.cwd?.() || '.', '');
	const backendTarget = env.BACKEND_URL || (globalThis as any).process?.env?.BACKEND_URL || 'http://127.0.0.1:8080';
	const adminToken = env.BAWAL_ADMIN_TOKEN || (globalThis as any).process?.env?.BAWAL_ADMIN_TOKEN || '';

	const configureProxy = (proxy: any) => {
		if (adminToken) {
			proxy.on('proxyReq', (proxyReq: any) => {
				proxyReq.setHeader('x-admin-token', adminToken);
			});
		}
	};

	return {
		plugins: [
			sveltekit()
		],
		server: {
			host: true,
			proxy: {
				'/api': {
					target: backendTarget,
					changeOrigin: true,
					secure: false,
					configure: configureProxy
				},
				'/examples': {
					target: backendTarget,
					changeOrigin: true,
					secure: false
				},
				'/inspections': {
					target: backendTarget,
					changeOrigin: true,
					secure: false,
					configure: configureProxy
				}
			}
		},
		preview: {
			host: true,
			proxy: {
				'/api': {
					target: backendTarget,
					changeOrigin: true,
					secure: false,
					configure: configureProxy
				},
				'/examples': {
					target: backendTarget,
					changeOrigin: true,
					secure: false
				},
				'/inspections': {
					target: backendTarget,
					changeOrigin: true,
					secure: false,
					configure: configureProxy
				}
			}
		}
	};
});
