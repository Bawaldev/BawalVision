# Module 01: Authentication & Session Cookie Lifecycle

This module verifies user account provisioning, password hashing with bcrypt, per-user cryptographic key generation (`HashKey` HMAC-SHA512 and `BlockKey` AES-256), and session cookie extraction.

---

## 1. User Registration

Registers a new user account and stores per-user security keys into PostgreSQL.

### Request
```bash
curl -i -X POST http://localhost:8080/api/v1/auth/register \
  -H "Content-Type: application/json" \
  -d '{
    "name": "BawalVision Engineer",
    "email": "engineer@bawalvision.ai",
    "password": "SecurePassword2026!"
  }'
```

### Expected Response (`201 Created`)
```json
{
  "message": "User registered successfully",
  "user_id": "8f3b1456-9812-4c6e-8d54-1b2c3d4e5f67"
}
```

---

## 2. User Login & Session Cookie Extraction

Authenticates credentials, returns user identity, and attaches an encrypted HTTP-only session cookie.

> **Note:** The `-c .cache_trash/cookies.txt` flag writes the received `Set-Cookie` header into a local cookie jar.

### Request
```bash
# Ensure cache directory exists
mkdir -p .cache_trash

curl -i -X POST http://localhost:8080/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{
    "email": "engineer@bawalvision.ai",
    "password": "SecurePassword2026!"
  }' \
  -c .cache_trash/cookies.txt
```

### Expected Headers
```http
HTTP/1.1 200 OK
Set-Cookie: session=...; Path=/; HttpOnly; Secure; SameSite=Lax
Content-Type: application/json
```

### Expected Response Body (`200 OK`)
```json
{
  "message": "Login successful",
  "user_id": "8f3b1456-9812-4c6e-8d54-1b2c3d4e5f67"
}
```

---

## 3. Inspecting the Cookie Jar

Verify that the secure session cookie has been persisted locally:

```bash
cat .cache_trash/cookies.txt
```

---

## 4. Negative Test: Invalid Credentials

Ensures unauthorized access attempts are rejected with `401 Unauthorized`.

```bash
curl -i -X POST http://localhost:8080/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{
    "email": "engineer@bawalvision.ai",
    "password": "IncorrectPassword!"
  }'
```

### Expected Response (`401 Unauthorized`)
```json
{
  "error": "Invalid email or password"
}
```

