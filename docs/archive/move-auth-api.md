# Move Authentication API

Discovered via probing http://move.local/

## API Endpoints

### Challenge Request

**Endpoint:** `POST http://move.local/api/v1/challenge`

**Request:**
```http
POST /api/v1/challenge HTTP/1.1
Host: move.local
Content-Type: application/json

{}
```

**Response:**
```http
HTTP/1.1 200 OK

{}
```

**Notes:**
- Triggers the Move to display a 6-digit code on its screen
- Must be called before submitting the code via challenge-response

### Code Submission

**Endpoint:** `POST http://move.local/api/v1/challenge-response`

**Request:**
```http
POST /api/v1/challenge-response HTTP/1.1
Host: move.local
Content-Type: application/json

{"secret": "123456"}
```

**Response (Invalid Code):**
```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json
X-Retries-Left: 2

{"error":"Invalid secret"}
```

**Response (Success):**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{"token": "69edab18782b04d75a70660e20d0042ab5c4273f04f91785d71cf3a7c911f510"}
```

**Response Headers:**
```
Set-Cookie: Ableton-Challenge-Response-Token=69edab18782b04d75a70660e20d0042ab5c4273f04f91785d71cf3a7c911f510; Path=/; ...
```

**Notes:**
- Parameter name is `secret`, not `code` or `response`
- Returns `X-Retries-Left` header showing remaining attempts
- Sets `Ableton-Challenge-Response-Token` cookie on success
- Response body includes the token value

### SSH Key Submission

**Endpoint:** `POST http://move.local/api/v1/ssh`

**Request (unauthenticated):**
```http
POST /api/v1/ssh HTTP/1.1
Host: move.local
Content-Type: application/x-www-form-urlencoded

ssh-ed25519 AAAA...
```

**Response (Unauthorized):**
```http
HTTP/1.1 400 Bad Request
Content-Type: application/json

{"error":"Invalid public SSH key"}
```

**Request (authenticated):**
```http
POST /api/v1/ssh HTTP/1.1
Host: move.local
Content-Type: application/x-www-form-urlencoded
Cookie: Ableton-Challenge-Response-Token=69edab18782b04d75a70660e20d0042ab5c4273f04f91785d71cf3a7c911f510

ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIH45y63MWZ5TPBQZqGkZDjcSHcevAp1UZjvMSp2xGDLy
```

**Response (Success):**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{"added":true}
```

**Notes:**
- Requires `Ableton-Challenge-Response-Token` cookie from challenge-response endpoint
- **Key format:** Send as raw POST body, NOT as JSON or form field
- **Content-Type:** `application/x-www-form-urlencoded` (even though it's raw data)
- **Remove comment:** Strip the `user@host` comment from the SSH key
- **Prefer ED25519:** Move accepts `ssh-ed25519` keys; RSA keys may fail
- Returns 400 with "Invalid public SSH key" if format is wrong

## Alternative Endpoint (Legacy?)

**Endpoint:** `POST http://move.local/development/ssh`

**Notes:**
- GET returns HTML (SPA routing)
- POST behavior needs testing - may be same as `/api/v1/ssh`

## Example curl Commands

### Submit code (test - will fail with fake code)
```bash
curl -v -X POST http://move.local/api/v1/challenge-response \
  -H "Content-Type: application/json" \
  -d '{"secret":"123456"}'
```

### Submit SSH key (after getting cookie)
```bash
curl -v -X POST http://move.local/api/v1/ssh \
  -H "Content-Type: application/json" \
  -H "Cookie: Ableton-Challenge-Response-Token=YOUR_TOKEN_HERE" \
  -d '{"publicKey":"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFoo... user@host"}'
```

## TODO: Validate with Real Code

To complete this documentation, test with an actual 6-digit code from Move screen:
1. Get code from Move display
2. Submit via `/api/v1/challenge-response`
3. Capture full `Set-Cookie` header
4. Test SSH key submission with cookie
5. Document success responses
