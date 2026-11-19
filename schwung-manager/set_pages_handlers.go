package main

import (
	"encoding/json"
	"net/http"
)

func (app *App) handleSetPages(w http.ResponseWriter, r *http.Request) {
	app.render(w, r, "set_pages.html", map[string]any{"Active": "set-pages"})
}
func (app *App) handleSetPagesRead(w http.ResponseWriter, r *http.Request) {
	s, err := readSetPages(app.basePath)
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(s)
}
func (app *App) handleSetPagesSave(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Revision string     `json:"revision"`
		Pages    [][]string `json:"pages"`
	}
	r.Body = http.MaxBytesReader(w, r.Body, 128*1024)
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid request", 400)
		return
	}
	if err := saveSetPages(app.basePath, req.Revision, req.Pages); err != nil {
		http.Error(w, err.Error(), http.StatusConflict)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
