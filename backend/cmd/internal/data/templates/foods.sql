CREATE EXTENSION IF NOT EXISTS "pgcrypto";

CREATE TABLE IF NOT EXISTS inspections (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    device_id VARCHAR(64) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    image_s3_uri TEXT NOT NULL,
    
    temperature NUMERIC(5, 2) NOT NULL,
    humidity NUMERIC(5, 2) NOT NULL,
    gas_voc NUMERIC(10, 2) NOT NULL,
    pressure NUMERIC(7, 2) NOT NULL,
    
    is_spoiled BOOLEAN NOT NULL,
    total_grams NUMERIC(7, 2) NOT NULL,
    t_safe_hours NUMERIC(5, 2) NOT NULL,
    
    foods JSONB NOT NULL DEFAULT '[]'::jsonb
);

CREATE INDEX IF NOT EXISTS idx_inspections_device_created 
    ON inspections(device_id, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_inspections_spoilage 
    ON inspections(is_spoiled) 
    WHERE is_spoiled = TRUE;
    
CREATE INDEX IF NOT EXISTS idx_inspections_foods_gin 
    ON inspections USING GIN (foods);