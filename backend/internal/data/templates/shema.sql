-- Active: 1789537729712@@127.0.0.1@5432@bawal_vision@public
CREATE EXTENSION IF NOT EXISTS "pgcrypto";
CREATE TABLE IF NOT EXISTS users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    hash_key BYTEA,
    block_key BYTEA,
    created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);
CREATE INDEX IF NOT EXISTS idx_users_deleted_at ON users(deleted_at);

CREATE TABLE IF NOT EXISTS inspections (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID REFERENCES users(id) ON DELETE SET NULL, -- Kolom user_id
    device_id VARCHAR(64) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    image_s3_uri TEXT,
    
    -- Embedded Telemetry Data
    temperature NUMERIC(5, 2) NOT NULL,
    humidity NUMERIC(5, 2) NOT NULL,
    gas_voc NUMERIC(10, 2) NOT NULL,
    pressure NUMERIC(7, 2) NOT NULL,
    cpu_load SMALLINT NOT NULL DEFAULT 0,
    
    -- Embedded Prediction Result
    is_spoiled BOOLEAN NOT NULL DEFAULT FALSE,
    total_grams NUMERIC(7, 2) NOT NULL DEFAULT 0.00,
    t_safe_hours NUMERIC(5, 2) NOT NULL DEFAULT 0.00,
    
    -- JSONB Array
    foods JSONB NOT NULL DEFAULT '[]'::jsonb
);

CREATE INDEX IF NOT EXISTS idx_inspections_user_id ON inspections(user_id);
CREATE INDEX IF NOT EXISTS idx_inspections_device_created ON inspections(device_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_inspections_spoilage ON inspections(is_spoiled) WHERE is_spoiled = TRUE;
CREATE INDEX IF NOT EXISTS idx_inspections_foods_gin ON inspections USING GIN (foods);