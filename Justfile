set dotenv-load := true
set shell := ["bash", "-uc"]

export WIFI_SSID   := env_var_or_default("WIFI_SSID", "")
export WIFI_PASS   := env_var_or_default("WIFI_PASS", "")
export BACKEND_IP  := env_var_or_default("BACKEND_IP", "")
export BACKEND_PORT:= env_var_or_default("BACKEND_PORT", "8080")
export IDF_PATH    := env_var_or_default("IDF_PATH", env_var("HOME") + "/esp/esp-idf")

# Import modul dari folder scripts/
import 'scripts/backend.just'
import 'scripts/edge.just'

default:
    @just --list