set dotenv-load := true
set shell := ["bash", "-uc"]

export IDF_PATH    := env_var_or_default("IDF_PATH", env_var("HOME") + "/esp/esp-idf")

# Import modules from scripts/ directory
import 'scripts/backend.just'
import 'scripts/edge.just'
import 'scripts/ml.just'

# Generate or refresh cryptographically secure secrets in .env
[group('Setup')]
setup:
    python3 scripts/generate_env.py

[private]
default:
    @just --list

