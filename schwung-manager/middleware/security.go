package middleware

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net/http"
	"path/filepath"
	"strings"
)

// ValidatePath resolves the given path and ensures it falls within one of the
// allowed root directories. It returns the cleaned absolute path or an error.
func ValidatePath(path string, allowedRoots []string) (string, error) {
	if path == "" {
		return "", fmt.Errorf("empty path")
	}

	cleaned := filepath.Clean(path)

	// Reject any path that still contains ".." after cleaning.
	if strings.Contains(cleaned, "..") {
		return "", fmt.Errorf("path contains disallowed traversal component")
	}

	// Must be absolute.
	if !filepath.IsAbs(cleaned) {
		return "", fmt.Errorf("path must be absolute")
	}

	for _, root := range allowedRoots {
		root = filepath.Clean(root)
		// Ensure the root ends with a separator so "/data/UserDataX" doesn't
		// match root "/data/UserData".
		rootPrefix := root
		if !strings.HasSuffix(rootPrefix, string(filepath.Separator)) {
			rootPrefix += string(filepath.Separator)
		}
		if cleaned == root || strings.HasPrefix(cleaned, rootPrefix) {
			return cleaned, nil
		}
	}

	return "", fmt.Errorf("path %q is not within any allowed root", cleaned)
}

// PathTraversalProtection returns middleware that validates the "path" query
// parameter and form value against the allowed root directories.
func PathTraversalProtection(allowedRoots []string) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			// Check query parameter.
			if p := r.URL.Query().Get("path"); p != "" {
				if _, err := ValidatePath(p, allowedRoots); err != nil {
					http.Error(w, "Forbidden: "+err.Error(), http.StatusForbidden)
					return
				}
			}

			// For POST requests, check form values if the form was already parsed
			// (by CSRF middleware). Don't call ParseForm on multipart — it may
			// interfere with file uploads.
			if r.Method == http.MethodPost && r.Form != nil {
				if p := r.FormValue("path"); p != "" {
					if _, err := ValidatePath(p, allowedRoots); err != nil {
						http.Error(w, "Forbidden: "+err.Error(), http.StatusForbidden)
						return
					}
				}
				if p := r.FormValue("dest"); p != "" {
					if _, err := ValidatePath(p, allowedRoots); err != nil {
						http.Error(w, "Forbidden: "+err.Error(), http.StatusForbidden)
						return
					}
				}
			}

			next.ServeHTTP(w, r)
		})
	}
}

// generateCSRFToken creates a cryptographically random hex token.
func generateCSRFToken() string {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		// Fallback — this should never happen.
		return "fallback-csrf-token"
	}
	return hex.EncodeToString(b)
}

// CSRFProtection is middleware that enforces a simple double-submit cookie
// pattern for state-changing requests.
//
// On every request it ensures a csrf_token cookie exists. For POST/PUT/DELETE
// requests it compares the cookie value against a form field or header named
// "csrf_token" / "X-CSRF-Token".
func CSRFProtection(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		cookie, err := r.Cookie("csrf_token")
		if err != nil || cookie.Value == "" {
			token := generateCSRFToken()
			http.SetCookie(w, &http.Cookie{
				Name:     "csrf_token",
				Value:    token,
				Path:     "/",
				HttpOnly: false, // JS needs to read it for HTMX
				SameSite: http.SameSiteStrictMode,
			})
			cookie = &http.Cookie{Name: "csrf_token", Value: token}
		}

		// For state-changing methods, verify the token.
		if r.Method == http.MethodPost || r.Method == http.MethodPut || r.Method == http.MethodDelete {
			var submitted string

			// Check header first (JS fetch / HTMX sends via header).
			submitted = r.Header.Get("X-CSRF-Token")

			// Fall back to form field.
			if submitted == "" {
				// Parse the form to get csrf_token field.
				if strings.HasPrefix(r.Header.Get("Content-Type"), "multipart/form-data") {
					r.ParseMultipartForm(64 << 20)
				} else {
					r.ParseForm()
				}
				submitted = r.FormValue("csrf_token")
			}

			if submitted == "" || submitted != cookie.Value {
				http.Error(w, "Forbidden: invalid CSRF token", http.StatusForbidden)
				return
			}
		}

		next.ServeHTTP(w, r)
	})
}
