package main

import (
	"encoding/json"
	"strings"
)

func jsonUnmarshal(b []byte, v any) error { return json.Unmarshal(b, v) }
func contains(s, sub string) bool         { return strings.Contains(s, sub) }
