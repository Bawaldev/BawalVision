# Install camera relay dependencies
[group('Relay')]
install-relay:
    cd {{ justfile_directory() }}/relay && npm install --no-audit --no-fund

# Run the WSS camera relay locally (reads RELAY_* from .env)
[group('Relay')]
run-relay:
    cd {{ justfile_directory() }}/relay && npm start

# Run relay unit tests
[group('Relay')]
test-relay:
    cd {{ justfile_directory() }}/relay && npm test

# Build the relay container image (context = repo root)
[group('Relay')]
build-relay:
    docker build -f {{ justfile_directory() }}/deploy/Dockerfile.relay -t bawal-camera-relay {{ justfile_directory() }}

# Install Cloudflare relay dependencies
[group('Relay')]
install-relay-cf:
    cd {{ justfile_directory() }}/relay-cloudflare && npm install --no-audit --no-fund

# Run the Cloudflare relay locally with wrangler (reads relay-cloudflare/.dev.vars)
[group('Relay')]
run-relay-cf:
    cd {{ justfile_directory() }}/relay-cloudflare && npm run dev

# Deploy the Cloudflare relay
[group('Relay')]
deploy-relay-cf:
    cd {{ justfile_directory() }}/relay-cloudflare && npm run deploy

# Smoke-test a running Cloudflare relay (start `just run-relay-cf` first)
[group('Relay')]
test-relay-cf:
    cd {{ justfile_directory() }}/relay-cloudflare && npm test

# Tail Cloudflare relay logs
[group('Relay')]
tail-relay-cf:
    cd {{ justfile_directory() }}/relay-cloudflare && npm run tail
