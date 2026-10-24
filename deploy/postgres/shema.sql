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

CREATE TABLE IF NOT EXISTS food_items (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) UNIQUE NOT NULL
);

CREATE TABLE IF NOT EXISTS inspections (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    device_id VARCHAR(64) NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    firmware_version VARCHAR(32) NOT NULL DEFAULT '1.0.0',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    image_s3_uri TEXT,
    cpu_temperature NUMERIC(5, 2) NOT NULL DEFAULT 35.00,
    temperature NUMERIC(5, 2) NOT NULL DEFAULT 25.00,
    cpu_load SMALLINT NOT NULL DEFAULT 0,
    is_spoiled BOOLEAN NOT NULL DEFAULT FALSE,
    t_safe_hours NUMERIC(5, 2) NOT NULL DEFAULT 0.00,
    frame_id BIGINT NOT NULL DEFAULT 0,
    lux_luminance NUMERIC(7, 2) NOT NULL DEFAULT 0.00,
    white_balance_status VARCHAR(32) NOT NULL DEFAULT 'BALANCED',
    color_temperature INT NOT NULL DEFAULT 6500,
    engine_pipeline VARCHAR(128) NOT NULL DEFAULT 'crow-cpp-inline-vision'
);

ALTER TABLE inspections ADD COLUMN IF NOT EXISTS firmware_version VARCHAR(32) NOT NULL DEFAULT '1.0.0';
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS image_s3_uri TEXT;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS cpu_temperature NUMERIC(5, 2) NOT NULL DEFAULT 35.00;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS temperature NUMERIC(5, 2) NOT NULL DEFAULT 25.00;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS cpu_load SMALLINT NOT NULL DEFAULT 0;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS is_spoiled BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS t_safe_hours NUMERIC(5, 2) NOT NULL DEFAULT 0.00;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS frame_id BIGINT NOT NULL DEFAULT 0;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS lux_luminance NUMERIC(7, 2) NOT NULL DEFAULT 0.00;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS white_balance_status VARCHAR(32) NOT NULL DEFAULT 'BALANCED';
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS color_temperature INT NOT NULL DEFAULT 6500;
ALTER TABLE inspections ADD COLUMN IF NOT EXISTS engine_pipeline VARCHAR(128) NOT NULL DEFAULT 'crow-cpp-inline-vision';

-- Drop legacy user_id and users table for open-source open web architecture
ALTER TABLE inspections DROP COLUMN IF EXISTS user_id;
DROP TABLE IF EXISTS users CASCADE;

<<<<<<< HEAD:backend/internal/data/templates/shema.sql
-- Drop legacy user_id and users table for open-source open web architecture
ALTER TABLE inspections DROP COLUMN IF EXISTS user_id;
DROP TABLE IF EXISTS users CASCADE;

=======
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444:deploy/postgres/shema.sql
CREATE INDEX IF NOT EXISTS idx_inspections_device_created ON inspections(device_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_inspections_spoilage_true ON inspections(created_at DESC) WHERE is_spoiled = TRUE;
CREATE INDEX IF NOT EXISTS idx_inspections_firmware ON inspections(firmware_version);

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = current_schema()
          AND table_name = 'inspections'
          AND column_name = 'food_temperature'
    ) THEN
        UPDATE inspections SET temperature = food_temperature;
        ALTER TABLE inspections DROP COLUMN food_temperature;
    END IF;
END $$;

CREATE TABLE IF NOT EXISTS inspection_foods (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    inspection_id UUID NOT NULL REFERENCES inspections(id) ON DELETE CASCADE,
    food_item_id INT NOT NULL REFERENCES food_items(id) ON DELETE RESTRICT,
    temperature NUMERIC(5, 2) NOT NULL DEFAULT 25.00,
    similarity_rate NUMERIC(5, 4) NOT NULL DEFAULT 0.0000,
    spoilage_score NUMERIC(5, 2) NOT NULL DEFAULT 0.00,
    status VARCHAR(16) NOT NULL DEFAULT 'PASS',
    CONSTRAINT uq_inspection_food UNIQUE (inspection_id, food_item_id)
);

ALTER TABLE inspection_foods DROP COLUMN IF EXISTS weight_grams;
ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS temperature NUMERIC(5, 2) NOT NULL DEFAULT 25.00;
ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS similarity_rate NUMERIC(5, 4) NOT NULL DEFAULT 0.0000;
ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS spoilage_score NUMERIC(5, 2) NOT NULL DEFAULT 0.00;
ALTER TABLE inspection_foods ADD COLUMN IF NOT EXISTS status VARCHAR(16) NOT NULL DEFAULT 'PASS';

CREATE INDEX IF NOT EXISTS idx_inspection_foods_inspection ON inspection_foods(inspection_id);
CREATE INDEX IF NOT EXISTS idx_inspection_foods_item ON inspection_foods(food_item_id);
CREATE INDEX IF NOT EXISTS idx_inspection_foods_status ON inspection_foods(status);
CREATE TABLE IF NOT EXISTS camera_frames (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    device_id VARCHAR(64) NOT NULL,
    frame_id BIGINT NOT NULL,
    width BIGINT NOT NULL DEFAULT 320,
    height BIGINT NOT NULL DEFAULT 240,
    format BIGINT NOT NULL DEFAULT 0,
    buffer_len BIGINT NOT NULL,
    timestamp_ms BIGINT NOT NULL,
    image_bytes BYTEA,
    image_s3_uri TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS idx_camera_frames_device_id ON camera_frames(device_id);
CREATE INDEX IF NOT EXISTS idx_camera_frames_deleted_at ON camera_frames(deleted_at);
