# schwung.local Reverse Proxy Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make Schwung Manager accessible at `schwung.local` (port 80, no port number) with screen mirroring at `schwung.local/mirror`, while preserving `move.local` for the stock Move web server.

**Architecture:** schwung-manager stays on port 7700. An iptables PREROUTING rule redirects incoming port 80 → 7700. The Go server inspects the Host header: `schwung.local` requests are served directly (or proxied to display-server for `/mirror`), everything else is reverse-proxied to `localhost:80` (stock Move server). mDNS advertisement for `schwung.local` uses `avahi-publish-host-name` in the entrypoint script.

**Tech Stack:** Go (net/http/httputil.ReverseProxy), iptables, avahi (mDNS), shell

---

### Task 1: Add Host-based routing and reverse proxy to schwung-manager

The Go server needs to wrap its existing mux with a top-level handler that inspects the Host header. If the host is NOT `schwung.local`, proxy the entire request to `localhost:80` (stock Move server). If it IS `schwung.local`, serve normally (existing routes). Additionally, `schwung.local/mirror` and `schwung.local/mirror/` should reverse-proxy to `localhost:7681` (display-server SSE stream).

**Files:**
- Modify: `schwung-manager/main.go`

**Step 1: Add reverse proxy handler**

In `main.go`, after the imports, add a `proxyHandler` function. This wraps the existing mux:

```go
// hostRouter returns a handler that routes based on Host header.
// Requests to schwungHost are served by schwungHandler.
// All other requests are reverse-proxied to the stock Move server on moveAddr.
func hostRouter(schwungHost string, schwungHandler http.Handler, moveAddr string, displayAddr string, logger *slog.Logger) http.Handler {
	moveProxy := &httputil.ReverseProxy{
		Director: func(req *http.Request) {
			req.URL.Scheme = "http"
			req.URL.Host = moveAddr
			req.Host = "" // let backend see original Host
		},
		ErrorHandler: func(w http.ResponseWriter, r *http.Request, err error) {
			logger.Error("move proxy error", "err", err)
			http.Error(w, "Stock Move server unavailable", http.StatusBadGateway)
		},
	}

	displayProxy := &httputil.ReverseProxy{
		Director: func(req *http.Request) {
			req.URL.Scheme = "http"
			req.URL.Host = displayAddr
			// Strip /mirror prefix
			req.URL.Path = strings.TrimPrefix(req.URL.Path, "/mirror")
			if req.URL.Path == "" {
				req.URL.Path = "/"
			}
		},
		// SSE streams need flushing
		FlushInterval: -1,
		ErrorHandler: func(w http.ResponseWriter, r *http.Request, err error) {
			logger.Error("display proxy error", "err", err)
			http.Error(w, "Display server unavailable", http.StatusBadGateway)
		},
	}

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		host := r.Host
		// Strip port if present
		if h, _, err := net.SplitHostPort(host); err == nil {
			host = h
		}

		if host == schwungHost {
			// /mirror → display server proxy
			if r.URL.Path == "/mirror" || strings.HasPrefix(r.URL.Path, "/mirror/") {
				displayProxy.ServeHTTP(w, r)
				return
			}
			schwungHandler.ServeHTTP(w, r)
			return
		}

		// Everything else → stock Move server
		moveProxy.ServeHTTP(w, r)
	})
}
```

**Step 2: Add import for net/http/httputil and net**

Add to the import block:
```go
"net"
"net/http/httputil"
```

**Step 3: Add CLI flags and wire up the router**

Add flags after the existing `flag.Parse()` block... actually, add the flags before `flag.Parse()`:

```go
schwungHost := flag.String("schwung-host", "schwung.local", "Hostname for Schwung Manager")
moveBackend := flag.String("move-backend", "127.0.0.1:80", "Address of stock Move web server")
displayBackend := flag.String("display-backend", "127.0.0.1:7681", "Address of display server")
```

Then, after the middleware is applied (after line `handler = middleware.CSRFProtection(handler)`), wrap with the host router:

```go
handler = hostRouter(*schwungHost, handler, *moveBackend, *displayBackend, logger)
```

**Step 4: Update the Screen Mirroring nav link**

In `schwung-manager/templates/base.html`, change the Screen Mirroring link from:
```html
<li><a href="http://move.local:7681" target="_blank" rel="noopener">Screen Mirroring</a></li>
```
to:
```html
<li><a href="/mirror" target="_blank" rel="noopener">Screen Mirroring</a></li>
```

This uses a relative path so it works on any hostname/port.

**Step 5: Commit**

```bash
git add schwung-manager/main.go schwung-manager/templates/base.html
git commit -m "feat: add Host-based reverse proxy for schwung.local and /mirror display proxy"
```

---

### Task 2: Add mDNS advertisement and iptables redirect to entrypoint

The entrypoint script needs to:
1. Publish `schwung.local` via avahi mDNS
2. Add an iptables PREROUTING rule to redirect port 80 → 7700

**Files:**
- Modify: `src/shim-entrypoint.sh`

**Step 1: Add iptables redirect and mDNS before schwung-manager startup**

Insert the following block BEFORE the schwung-manager startup line (before line 62 `# Start schwung-manager web UI if present`):

```bash
# === Set up schwung.local reverse proxy ===
# Redirect incoming port 80 to schwung-manager (port 7700) for Host-based routing.
# PREROUTING only affects external packets; loopback connections to localhost:80
# (used by the reverse proxy to reach the stock Move server) are not affected.
if command -v iptables >/dev/null 2>&1; then
    # Remove any stale rule first, then add
    iptables -t nat -D PREROUTING -p tcp --dport 80 -j REDIRECT --to-port 7700 2>/dev/null || true
    iptables -t nat -A PREROUTING -p tcp --dport 80 -j REDIRECT --to-port 7700
fi

# Advertise schwung.local via mDNS (avahi).
# Get the device's IP from the default route interface.
if command -v avahi-publish-host-name >/dev/null 2>&1; then
    # Kill any previous instance
    pkill -f 'avahi-publish-host-name schwung.local' 2>/dev/null || true
    DEVICE_IP=$(ip -4 route get 1 2>/dev/null | awk '{print $7; exit}')
    if [ -n "$DEVICE_IP" ]; then
        avahi-publish-host-name schwung.local "$DEVICE_IP" &
    fi
elif command -v avahi-publish >/dev/null 2>&1; then
    pkill -f 'avahi-publish.*schwung' 2>/dev/null || true
    DEVICE_IP=$(ip -4 route get 1 2>/dev/null | awk '{print $7; exit}')
    if [ -n "$DEVICE_IP" ]; then
        avahi-publish -a schwung.local "$DEVICE_IP" &
    fi
fi
```

**Step 2: Commit**

```bash
git add src/shim-entrypoint.sh
git commit -m "feat: add iptables port 80 redirect and mDNS schwung.local advertisement"
```

---

### Task 3: Handle edge cases and cleanup

**Files:**
- Modify: `schwung-manager/main.go` (minor)
- Modify: `src/shim-entrypoint.sh` (minor)

**Step 1: Handle SSE streaming for /mirror proxy**

The display-server sends SSE (Server-Sent Events). The reverse proxy must not buffer the response. The `FlushInterval: -1` in Task 1 handles this, but we also need to ensure the proxy copies hop-by-hop headers correctly for SSE. Verify that `Content-Type: text/event-stream` passes through.

The display-server serves both the SSE stream AND the HTML page. Check what paths it uses:

Looking at `src/host/display_server.c`, the server handles:
- `GET /` — serves an HTML page with embedded JS that connects to `/events`
- `GET /events` — SSE stream of base64 display frames

So `/mirror` should proxy to `/` (HTML page) and `/mirror/events` should proxy to `/events`. The path stripping in Task 1's `displayProxy` handles this: `strings.TrimPrefix(req.URL.Path, "/mirror")` turns `/mirror` → `/` and `/mirror/events` → `/events`.

However, the HTML page served by display-server will have `new EventSource('/events')` hardcoded. When accessed via `/mirror`, the browser will try to connect to `/events` (not `/mirror/events`), which would hit the schwung-manager routes instead of the display proxy.

**Fix:** We need to either:
(a) Rewrite the HTML response to change `/events` to `/mirror/events`
(b) Also route `/events` to the display proxy when Host is `schwung.local`

Option (b) is simpler — add `/events` as another display proxy route:

In the `hostRouter` function, update the schwung.local branch:
```go
if host == schwungHost {
    // /mirror and /events → display server proxy
    if r.URL.Path == "/mirror" || strings.HasPrefix(r.URL.Path, "/mirror/") ||
       r.URL.Path == "/events" {
        displayProxy.ServeHTTP(w, r)
        return
    }
    schwungHandler.ServeHTTP(w, r)
    return
}
```

And update the Director to also handle `/events` without stripping:
```go
Director: func(req *http.Request) {
    req.URL.Scheme = "http"
    req.URL.Host = displayAddr
    if strings.HasPrefix(req.URL.Path, "/mirror") {
        req.URL.Path = strings.TrimPrefix(req.URL.Path, "/mirror")
        if req.URL.Path == "" {
            req.URL.Path = "/"
        }
    }
    // /events passes through as-is
},
```

**Step 2: Add cleanup of iptables rule on uninstall**

In `scripts/uninstall.sh`, add cleanup of the iptables rule and mDNS process. Find the file and add before service restart:

```bash
# Remove schwung.local iptables redirect
iptables -t nat -D PREROUTING -p tcp --dport 80 -j REDIRECT --to-port 7700 2>/dev/null || true
# Stop mDNS advertisement
pkill -f 'avahi-publish.*schwung' 2>/dev/null || true
```

**Step 3: Commit**

```bash
git add schwung-manager/main.go scripts/uninstall.sh
git commit -m "fix: handle /events SSE path for display mirror proxy, cleanup on uninstall"
```

---

## Summary

| Task | What | Files |
|------|------|-------|
| 1 | Host-based reverse proxy in Go + /mirror route + nav link update | `main.go`, `base.html` |
| 2 | iptables redirect + mDNS advertisement in entrypoint | `shim-entrypoint.sh` |
| 3 | SSE /events path fix + uninstall cleanup | `main.go`, `uninstall.sh` |

After deployment:
- `schwung.local` → Schwung Manager (modules, files, config, system)
- `schwung.local/mirror` → Screen Mirroring
- `move.local` → Stock Move web server (proxied through)
- `move.local:7700` → Schwung Manager (still works directly)
