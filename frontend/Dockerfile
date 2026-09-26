# ==========================================
# Frontend Build Stage (SvelteKit / Vite)
# ==========================================
FROM node:22-alpine AS builder

WORKDIR /app

# Copy dependency specifications first for Docker layer caching
COPY package.json package-lock.json ./
RUN npm ci

# Copy application source
COPY . .

# Build SvelteKit static / SSR assets
RUN npm run build

# ==========================================
# Frontend Production Stage
# ==========================================
FROM node:22-alpine AS runner

WORKDIR /app

# Copy package manifests and installed dependencies
COPY --from=builder /app/package.json /app/package-lock.json ./
COPY --from=builder /app/node_modules ./node_modules
COPY --from=builder /app/.svelte-kit ./.svelte-kit
COPY --from=builder /app/static ./static
COPY --from=builder /app/src ./src
COPY --from=builder /app/vite.config.ts /app/tsconfig.json ./

ENV HOST=0.0.0.0 \
    PORT=3000 \
    NODE_ENV=production

EXPOSE 3000

CMD ["npm", "run", "preview", "--", "--host", "0.0.0.0", "--port", "3000"]
