set dotenv-load := true
set shell := ["bash", "-uc"]

export IDF_PATH    := env_var_or_default("IDF_PATH", env_var("HOME") + "/esp/esp-idf")

# Import modul dari folder scripts/
import 'scripts/backend.just'
import 'scripts/edge.just'

default:
    @just --list