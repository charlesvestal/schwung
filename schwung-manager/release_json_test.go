package main

import (
	"encoding/json"
	"strings"
	"testing"
)

func decodeReleaseJSON(t *testing.T, value string) ReleaseJSON {
	t.Helper()
	var release ReleaseJSON
	if err := json.NewDecoder(strings.NewReader(value)).Decode(&release); err != nil {
		t.Fatalf("decode release.json: %v", err)
	}
	return release
}

func TestReleaseJSONForModuleSingle(t *testing.T) {
	release := decodeReleaseJSON(t, `{
		"version":"1.2.3",
		"download_url":"https://example.invalid/single.tar.gz"
	}`)

	got, ok := release.forModule("anything")
	if !ok {
		t.Fatal("single-module release unexpectedly rejected")
	}
	if got.Version != "1.2.3" || got.DownloadURL != "https://example.invalid/single.tar.gz" {
		t.Fatalf("got %+v", got)
	}
}

func TestReleaseJSONForModuleMulti(t *testing.T) {
	release := decodeReleaseJSON(t, `{
		"modules":{
			"mono":{"version":"0.3.1","download_url":"https://example.invalid/mono.tar.gz"},
			"mono-voice":{"version":"0.3.1","download_url":"https://example.invalid/mono-voice.tar.gz"}
		}
	}`)

	got, ok := release.forModule("mono-voice")
	if !ok {
		t.Fatal("multi-module release entry not found")
	}
	if got.Version != "0.3.1" || got.DownloadURL != "https://example.invalid/mono-voice.tar.gz" {
		t.Fatalf("got %+v", got)
	}

	if _, ok := release.forModule("missing"); ok {
		t.Fatal("missing multi-module entry unexpectedly accepted")
	}
}
