// schwung-manager/boot_page_test.go
package main

import "testing"

func TestBootPageRowOrdering(t *testing.T) {
	rows := bootPageRows([]registryEntry{
		{ID: "zeta", Name: "Zeta", Owner: "platform:zeta"},
		{ID: "alpha", Name: "Alpha", Owner: "module:alpha"},
		{ID: "schwung", Name: "Schwung"},
	}, "alpha")

	var got []string
	for _, r := range rows {
		got = append(got, r.ID)
	}
	want := []string{"schwung", "alpha", "zeta", "stock"}
	if len(got) != len(want) {
		t.Fatalf("rows = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("rows = %v, want %v", got, want)
		}
	}
	for _, r := range rows {
		if r.ID == "alpha" && !r.IsDefault {
			t.Error("current default not marked")
		}
	}
}

func TestBootPageSourceLabels(t *testing.T) {
	rows := bootPageRows([]registryEntry{
		{ID: "a", Name: "A", Owner: "module:amod"},
		{ID: "b", Name: "B", Owner: "platform:bplat"},
		{ID: "c", Name: "C"},
	}, "")
	want := map[string]string{
		"a": "module: amod", "b": "platform: bplat", "c": "installed manually",
	}
	for _, r := range rows {
		if w, ok := want[r.ID]; ok && r.Source != w {
			t.Errorf("row %s source = %q, want %q", r.ID, r.Source, w)
		}
	}
}

func TestBootDefaultRefusesUnregistered(t *testing.T) {
	app, _, reg := newReconcileApp(t)
	if err := app.setBootDefault("ghost"); err == nil {
		t.Fatal("want a refusal for an unregistered id")
	}
	if got, _ := readBootDefault(reg); got != "" {
		t.Errorf("default was written anyway: %q", got)
	}
	if err := app.setBootDefault("stock"); err != nil {
		t.Fatalf("stock is always valid: %v", err)
	}
}
