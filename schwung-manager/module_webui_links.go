package main

import (
	"fmt"
	"net/url"
	"os"
	"path/filepath"
)

// A module's web_ui.html is not a page about the module, it is a page about
// ONE PLACE the module is loaded. schwung-remote-api.js learns that place from
// the query string the Remote UI builds -- slot + component, tool=1 or
// master_fx=1 -- and with none it falls back to slot 0 as "synth". Linked
// bare from the module page, every web UI in the fleet therefore drove Track
// 1's synth whatever was loaded there: the JV-880 page writing jv keys into an
// OB-Xd, an audio FX page writing "synth:mix". So the module page links one
// button per place the module is actually loaded, with the same query the
// Remote UI's own pop-out button builds (makePopOutButton, remote-ui.js).

// WebUILink is one place a module's web UI can be opened against.
type WebUILink struct {
	Where string // "Track 2 Synth"; empty for a DSP-less overtake module, which has no place
	URL   string
}

// WebUILinks is what the module page is told. Unknown means a read did not
// complete: the placements found so far are withheld rather than shown as the
// whole answer, since a module missing from the list would read as "not
// loaded anywhere".
type WebUILinks struct {
	Present bool // the module ships a web_ui.html at all
	Links   []WebUILink
	Unknown bool
}

// paramGetter reads one shim param; the ShmParams method in production, a map
// in tests.
type paramGetter func(slot uint8, key string) (string, error)

const webUIChainSlots = 4

var webUIComponentLabels = map[string]string{
	"synth":    "Synth",
	"fx1":      "FX 1",
	"fx2":      "FX 2",
	"midi_fx1": "MIDI FX",
}

// webUISearchScope says which positions a module of this component_type can
// occupy, so a synth's page costs 4 reads rather than 21. Each read is a
// round-trip through the param channel and a busy one blocks up to its
// timeout, so reading positions a module cannot be in is not free. An
// unknown or empty type searches everything.
func webUISearchScope(componentType string) (comps []string, masterFx, tool bool) {
	switch componentType {
	case "sound_generator":
		return []string{"synth"}, false, false
	case "audio_fx":
		return []string{"fx1", "fx2"}, true, false
	case "midi_fx":
		return []string{"midi_fx1"}, false, false
	case "tool", "overtake":
		return nil, false, true
	}
	return componentPrefixes, true, true
}

// findModuleWebUILinks lists every place moduleID is loaded, each with the
// URL that opens its web UI against that place. baseURL is the module's
// web_ui.html asset URL. get may be nil (no param channel), which is Unknown.
func findModuleWebUILinks(get paramGetter, moduleID, componentType, baseURL string) WebUILinks {
	unknown := WebUILinks{Present: true, Unknown: true}
	if get == nil {
		return unknown
	}
	comps, masterFx, tool := webUISearchScope(componentType)
	out := WebUILinks{Present: true}

	// read answers (value, ok). ok=false is a read that did not complete, and
	// ends the search: see WebUILinks.Unknown. An ANSWERED error (the shim
	// replied "no such param") is a real "nothing here".
	read := func(slot uint8, key string) (string, bool) {
		v, err := get(slot, key)
		if err != nil {
			return "", paramAnswered(err)
		}
		return v, true
	}
	link := func(where, query string) {
		out.Links = append(out.Links, WebUILink{Where: where, URL: baseURL + "?" + query})
	}

	for slot := uint8(0); slot < webUIChainSlots; slot++ {
		for _, comp := range comps {
			v, ok := read(slot, comp+"_module")
			if !ok {
				return unknown
			}
			if v != moduleID {
				continue
			}
			link(fmt.Sprintf("Track %d %s", slot+1, webUIComponentLabels[comp]),
				"component="+url.QueryEscape(comp)+
					"&schwungStandalone=1&slot="+fmt.Sprint(slot))
		}
	}
	if masterFx {
		for i, fx := range masterFxSlots {
			v, ok := read(0, "master_fx:"+fx+":module")
			if !ok {
				return unknown
			}
			if masterFxModuleID(v) != moduleID {
				continue
			}
			link(fmt.Sprintf("Master FX %d", i+1),
				"component="+url.QueryEscape("master_fx:"+fx)+
					"&schwungStandalone=1&master_fx=1")
		}
	}
	if tool {
		v, ok := read(0, overtakeParamPrefix+"module_id")
		if !ok {
			return unknown
		}
		if v == moduleID {
			link("Tool", "schwungStandalone=1&tool=1")
		}
	}
	return out
}

// moduleHasDSP reports whether a module directory carries a DSP plugin.
func moduleHasDSP(modDir string) bool {
	matches, _ := filepath.Glob(filepath.Join(modDir, "*.so"))
	return len(matches) > 0
}

// moduleWebUILinks is the module page's answer: nothing when the module ships
// no web_ui.html, otherwise its placements.
//
// One case needs no placement. An overtake module with no DSP -- all ui.js,
// the case #512 was written for -- has nothing a param could address, and the
// overtake_dsp:module_id probe cannot see it, because that probe asks the DSP.
// Its page is opened on the tool channel, as the Tool tab's pop-out is: it
// subscribes to no chain slot and is told no chain component, so nothing
// points it at Track 1's synth. (A page that writes a bare "synth:" key on
// that channel still lands on slot 0 -- the channel routes by key -- but that
// is the page naming a synth, not a default choosing one for it.)
func (app *App) moduleWebUILinks(moduleID, componentType, modDir string) WebUILinks {
	if modDir == "" {
		return WebUILinks{}
	}
	if _, err := os.Stat(filepath.Join(modDir, "web_ui.html")); err != nil {
		return WebUILinks{}
	}
	baseURL := "/api/remote-ui/module-assets/" + url.PathEscape(moduleID) + "/web_ui.html"

	if (componentType == "overtake" || componentType == "tool") && !moduleHasDSP(modDir) {
		return WebUILinks{Present: true, Links: []WebUILink{{URL: baseURL + "?schwungStandalone=1&tool=1"}}}
	}

	var get paramGetter
	if shm := app.params(); shm != nil {
		get = shm.GetParam
	}
	return findModuleWebUILinks(get, moduleID, componentType, baseURL)
}
