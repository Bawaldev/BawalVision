-- Active: 1789537729712@@127.0.0.1@5432@bawal_vision@public
CREATE EXTENSION IF NOT EXISTS "pgcrypto";

CREATE TABLE IF NOT EXISTS devices (
    id VARCHAR(64) PRIMARY KEY,
    device_type VARCHAR(50) NOT NULL DEFAULT 'ESP32',
    device_core SMALLINT NOT NULL DEFAULT 2 CHECK (device_core > 0),
    firmware_version VARCHAR(32) NOT NULL DEFAULT '1.0.0',
    kitchen_id VARCHAR(64),
    status VARCHAR(20) NOT NULL DEFAULT 'ACTIVE',
    last_seen_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_devices_kitchen_id ON devices(kitchen_id);
CREATE INDEX IF NOT EXISTS idx_devices_last_seen ON devices(last_seen_at DESC);
CREATE INDEX IF NOT EXISTS idx_devices_status ON devices(status);

CREATE TABLE IF NOT EXISTS users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    hash_key BYTEA,
    block_key BYTEA,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);
CREATE INDEX IF NOT EXISTS idx_users_deleted_at ON users(deleted_at);

CREATE TABLE IF NOT EXISTS food_items (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) UNIQUE NOT NULL
);

CREATE TABLE IF NOT EXISTS inspections (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID REFERENCES users(id) ON DELETE SET NULL,
    device_id VARCHAR(64) NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    firmware_version VARCHAR(32) NOT NULL DEFAULT '1.0.0',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    image_s3_uri TEXT,
    temperature NUMERIC(5, 2) NOT NULL,
    humidity NUMERIC(5, 2) NOT NULL,
    gas_voc NUMERIC(10, 2) NOT NULL,
    pressure NUMERIC(7, 2) NOT NULL,
    cpu_load SMALLINT NOT NULL DEFAULT 0,
    is_spoiled BOOLEAN NOT NULL DEFAULT FALSE,
    t_safe_hours NUMERIC(5, 2) NOT NULL DEFAULT 0.00
);

ALTER TABLE inspections ADD COLUMN IF NOT EXISTS firmware_version VARCHAR(32) NOT NULL DEFAULT '1.0.0';
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS user_id UUID REFERENCES users(id) ON DELETE SET NULL;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS image_s3_uri TEXT;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS cpu_load SMALLINT NOT NULL DEFAULT 0;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS is_spoiled BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS t_safe_hours NUMERIC(5, 2) NOT NULL DEFAULT 0.00;

CREATE INDEX IF NOT EXISTS idx_inspections_user_id ON inspections(user_id);
CREATE INDEX IF NOT EXISTS idx_inspections_device_created ON inspections(device_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_inspections_spoilage_true ON inspections(created_at DESC) WHERE is_spoiled = TRUE;
CREATE INDEX IF NOT EXISTS idx_inspections_firmware ON inspections(firmware_version);

CREATE TABLE IF NOT EXISTS inspection_foods (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    inspection_id UUID NOT NULL REFERENCES inspections(id) ON DELETE CASCADE,
    food_item_id INT NOT NULL REFERENCES food_items(id) ON DELETE RESTRICT,
    weight_grams NUMERIC(7, 2) NOT NULL DEFAULT 0.00,
    CONSTRAINT uq_inspection_food UNIQUE (inspection_id, food_item_id)
);

CREATE INDEX IF NOT EXISTS idx_inspection_foods_inspection ON inspection_foods(inspection_id);
CREATE INDEX IF NOT EXISTS idx_inspection_foods_item ON inspection_foods(food_item_id);