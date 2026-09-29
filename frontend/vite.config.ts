import adapter from '@sveltejs/adapter-auto';
import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig, loadEnv } from 'vite';

export default defineConfig(({ mode }) => {
	const env = loadEnv(mode, (globalThis as any).process?.cwd?.() || '.', '');
	const backendTarget = env.BACKEND_URL || (globalThis as any).process?.env?.BACKEND_URL || 'http://backend:8080';

	return {
		plugins: [
			sveltekit({
				compilerOptions: {
					runes: ({ filename }) =>
						filename.split(/[/\\]/).includes('node_modules') ? undefined : true
				},
				adapter: adapter()
			})
		],
		server: {
			proxy: {
				'/api': {
					target: env.BACKEND_URL || (globalThis as any).process?.env?.BACKEND_URL || 'http://127.0.0.1:8080',
					changeOrigin: true
				}
			}
		},
		preview: {
			proxy: {
				'/api': {
					target: backendTarget,
					changeOrigin: true
				}
			}
		}
	};
});
