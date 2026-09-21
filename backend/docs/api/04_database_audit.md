# Module 04: Database Integrity & Direct SQL Audit

This module provides direct PostgreSQL verification queries to audit database consistency, table schemas, cryptographic key generation, foreign key constraints, and performance indexes.

---

## 1. PostgreSQL Container Connection

Access the PostgreSQL container shell via Docker:

```bash
docker exec -it postgres psql -U postgres -d bawal_vision
```

---

## 2. Table Schema & Migration Audit

Verify that all required entity tables have been migrated properly:

```sql
-- List all tables in public schema
\dt

-- Expected tables:
--  - users
--  - devices
--  - food_items
--  - inspections
--  - inspection_foods
```

---

## 3. Cryptographic Key Storage & User Audit

Verify that users have valid cryptographic byte arrays generated for `hash_key` and `block_key`:

```sql
SELECT 
    id, 
    name, 
    email, 
    octet_length(hash_key)  AS hash_key_bytes,   -- Expected: 64 bytes (HMAC-SHA512)
    octet_length(block_key) AS block_key_bytes,  -- Expected: 32 bytes (AES-256)
    created_at
FROM users
ORDER BY created_at DESC;
```

---

## 4. Inspection Telemetry Logs Audit

Query the latest environmental telemetry entries inserted from edge devices:

```sql
SELECT 
    id,
    device_id,
    temperature,
    humidity,
    gas_voc,
    pressure,
    cpu_load,
    is_spoiled,
    t_safe_hours,
    created_at
FROM inspections
ORDER BY created_at DESC
LIMIT 10;
```

---

## 5. Clean-Up Test Records (Optional)

To purge temporary test telemetry entries generated during benchmarking:

```sql
DELETE FROM inspections WHERE device_id LIKE '%TEST%';
```

