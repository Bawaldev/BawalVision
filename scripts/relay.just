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
