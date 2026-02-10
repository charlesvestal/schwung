# Move Authentication API

Discovered via probing http://move.local/

## API Endpoints

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
Set-Cookie: Ableton-Challenge-Response-Token=[value]; Path=/; ...

[Success response - need to test with valid code]
```

**Notes:**
- Parameter name is `secret`, not `code`
- Returns `X-Retries-Left` header showing remaining attempts
- Sets `Ableton-Challenge-Response-Token` cookie on success

### SSH Key Submission

**Endpoint:** `POST http://move.local/api/v1/ssh`

**Request (unauthenticated):**
```http
POST /api/v1/ssh HTTP/1.1
Host: move.local
Content-Type: application/json

{"publicKey":"ssh-ed25519 AAAA..."}
```

**Response (Unauthorized):**
```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json

{"error":"Unset credentials"}
```

**Request (authenticated):**
```http
POST /api/v1/ssh HTTP/1.1
Host: move.local
Content-Type: application/json
Cookie: Ableton-Challenge-Response-Token=[value]

{"publicKey":"ssh-ed25519 AAAA... user@host"}
```

**Response (Success - needs testing):**
```http
HTTP/1.1 200 OK
Content-Type: application/json

[Success response]
```

**Notes:**
- Requires `Ableton-Challenge-Response-Token` cookie from challenge-response endpoint
- Parameter name is `publicKey` (camelCase)
- Returns 401 with "Unset credentials" if no auth cookie provided

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
