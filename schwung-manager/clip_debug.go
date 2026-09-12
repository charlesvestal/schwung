package main

// Clip-state debug page. Move's sequencer emits no CC; the shim decodes which
// clip is playing on each track from Move's own cable-0 LED stream. This
// serves that decode live so it can be watched against what the device is
// visibly doing -- which is the only way to tell a correct decode from one
// that merely agrees with itself.
//
// Arm it on the device with:  touch /data/UserData/schwung/clip_state_on

import (
	"net/http"
	"os"
)

const (
	clipStatePath = "/data/UserData/schwung/clip_state.json"
	clipArmPath   = "/data/UserData/schwung/clip_state_on"
)

// GET /api/clip-state — the shim's snapshot, passed through verbatim.
// Reports "armed" separately so the page can tell "not armed" from "armed but
// producing nothing", which look identical from an empty body and mean very
// different things.
func (a *App) handleAPIClipState(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	armed := false
	if _, err := os.Stat(clipArmPath); err == nil {
		armed = true
	}
	body, err := os.ReadFile(clipStatePath)
	if err != nil || len(body) == 0 {
		if armed {
			w.Write([]byte(`{"armed":true,"waiting":true}`))
		} else {
			w.Write([]byte(`{"armed":false}`))
		}
		return
	}
	// Splice "armed" into the shim's object without parsing it.
	out := append([]byte(`{"armed":true,`), body[1:]...)
	w.Write(out)
}

// POST /clip-state/arm — create or remove the arming file.
func (a *App) handleClipStateArm(w http.ResponseWriter, r *http.Request) {
	if r.FormValue("on") == "1" {
		f, err := os.Create(clipArmPath)
		if err == nil {
			f.Close()
		}
	} else {
		os.Remove(clipArmPath)
		// Remove the snapshot too, so a stale state cannot be mistaken for a
		// live one next time the page is opened.
		os.Remove(clipStatePath)
	}
	http.Redirect(w, r, "/clip-state", http.StatusSeeOther)
}

func (a *App) handleClipState(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(clipStateHTML))
}

const clipStateHTML = `<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clip State</title>
<style>
 body{font:14px system-ui,-apple-system,sans-serif;margin:0;padding:16px;
      background:#111;color:#eee}
 h1{font-size:18px;margin:0 0 4px}
 p.sub{color:#888;margin:0 0 16px}
 table{border-collapse:collapse;width:100%;max-width:640px}
 th,td{text-align:left;padding:8px 10px;border-bottom:1px solid #282828}
 th{color:#888;font-weight:500;font-size:12px;text-transform:uppercase;
    letter-spacing:.04em}
 td.n{font-variant-numeric:tabular-nums}
 .pill{display:inline-block;padding:2px 8px;border-radius:10px;font-size:12px}
 .ok{background:#12351e;color:#6ee7a0}
 .warn{background:#3a3115;color:#f0c96a}
 .off{background:#2a2a2a;color:#888}
 .bar{color:#666;margin-top:14px;font-size:12px}
 button{font:inherit;padding:6px 14px;border-radius:6px;border:1px solid #444;
        background:#1e1e1e;color:#eee;cursor:pointer}
 .ctx{margin:10px 0 18px;color:#aaa;font-size:13px}
 .ctx b{color:#eee;font-weight:600}
 .g{border-collapse:separate;border-spacing:4px}
 .g td{width:34px;height:30px;text-align:center;border-radius:5px;
       background:#1a1a1a;color:#555;font-size:11px;border:1px solid #222;padding:0}
 .g td.ex{color:#999;border-color:#333}
 .g td.sel{background:#20304a;color:#9bc0f0;border-color:#37527e}
 .g td.live{background:#12351e;color:#6ee7a0;border-color:#276b42;font-weight:600}
 .g th{color:#666;font-weight:500;font-size:11px;padding:0 4px}
 .k{display:inline-block;padding:1px 6px;border-radius:4px;font-size:11px}
 .k.live{background:#12351e;color:#6ee7a0}
 .k.sel{background:#20304a;color:#9bc0f0}
 .k.ex{background:#1a1a1a;color:#999;border:1px solid #333}
</style></head><body>
<h1>Clip State</h1>
<p class="sub">What the shim has decoded from Move&rsquo;s LED stream and Song.abl. Updates ~1&nbsp;Hz.</p>
<div id="armbox"></div>
<div id="ctx" class="ctx"></div>
<table><thead><tr><th>Track</th><th>Clip</th><th>Loop</th><th>Phase</th><th>Position</th></tr></thead>
<tbody id="rows"></tbody></table>
<div class="bar" id="bar"></div>
<h2 style="font-size:14px;margin:22px 0 6px">Phase check</h2>
<p class="sub" style="margin:0 0 10px">Our computed phase vs Move&rsquo;s own step
 playhead &mdash; an independent measure. The step editor shows one track, so
 only that one should score. <b>A one-beat error scores ~0, not 90%</b>.</p>
<div id="pc" class="ctx"></div>

<h2 style="font-size:14px;margin:22px 0 6px">Grid as decoded</h2>
<p class="sub" style="margin:0 0 10px">Rows are tracks, columns clips 1&ndash;8.
 <span class="k live">live</span> what we think is playing &middot;
 <span class="k sel">file</span> the selection Song.abl restored &middot;
 <span class="k ex">&middot;</span> a clip exists &middot; blank = empty</p>
<div id="grid"></div>
<script>
function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
async function tick(){
  let d;
  try { d = await (await fetch('/api/clip-state',{cache:'no-store'})).json(); }
  catch(e){ document.getElementById('bar').textContent='(manager unreachable)'; return; }
  const arm=document.getElementById('armbox');
  if(!d.armed){
    arm.innerHTML='<form method="post" action="/clip-state/arm">'+
      '<input type="hidden" name="on" value="1">'+
      '<button>Start tracking</button></form>'+
      '<p class="sub" style="margin-top:10px">Not armed &mdash; the shim is not decoding.</p>';
    document.getElementById('rows').innerHTML='';
    document.getElementById('bar').textContent='';
    return;
  }
  arm.innerHTML='<form method="post" action="/clip-state/arm">'+
    '<input type="hidden" name="on" value="0">'+
    '<button>Stop tracking</button></form>';
  if(d.waiting || !d.tracks){
    document.getElementById('rows').innerHTML='';
    document.getElementById('bar').textContent='armed — waiting for the first LED scan';
    return;
  }
  document.getElementById('rows').innerHTML=d.tracks.map(t=>{
    let clip, phase, el;
    if(!t.known){ clip='<span class="pill off">unknown</span>'; phase=''; el=''; }
    else if(t.clip===0){ clip='<span class="pill off">nothing playing</span>'; phase=''; el=''; }
    else {
      clip='clip '+esc(t.clip);
      /* "unknown" is a third answer, not zero -- a lane must refuse to record
         here rather than record at a guessed phase. */
      phase = t.anchored ? '<span class="pill ok">anchored</span>'
                         : '<span class="pill warn">phase unknown</span>';
      /* Position WITHIN the loop, never the absolute phase against the loop
         LENGTH -- a clip whose loop starts at beat 8 runs 8..16, and printing
         "12.00 / 8.00" reads as nonsense. The absolute figure is kept in the
         title for when it is the one you want. */
      if(t.has_phase) el = '<span title="absolute '+(+t.phase).toFixed(2)+'">'+
        (+t.pos).toFixed(2)+' / '+(+t.loop_len).toFixed(2)+'</span>';
      else if(t.anchored) el = '+'+(+t.elapsed_beats).toFixed(2)+' (no loop len)';
      else el = '\u2014';
    }
    const loop = (t.known && t.clip && t.loop_len)
      ? (+t.loop_len).toFixed(2)+' beats'+((+t.loop_start)?' from '+(+t.loop_start).toFixed(2):'')
      : '\u2014';
    return '<tr><td>'+esc(t.track)+'</td><td>'+clip+'</td><td class="n">'+loop+
           '</td><td>'+phase+'</td><td class="n">'+el+'</td></tr>';
  }).join('');
  document.getElementById('bar').textContent =
    'pulse '+d.pulses+'  \u00b7  beat '+(+d.beat).toFixed(2);

  const MODES={0:'unknown',1:'Session',2:'Note',3:'Set Overview'};
  /* The mode is shown because the clip gate depends on it: outside Session
     the pads are not clips, and the rejection is silent. */
  document.getElementById('ctx').innerHTML =
    'Set <b>'+esc(d.set||'?')+'</b> \u00b7 Move UI mode <b>'+
    esc(MODES[d.ui_mode]!==undefined?MODES[d.ui_mode]:d.ui_mode)+'</b>'+
    (d.ui_mode===1?'':' <span class="k sel">pads are not clips in this mode</span>')+
    ' \u00b7 Song.abl '+(d.regions_valid?'loaded':'<b>not loaded</b>');

  const pc=d.phase_check;
  if(pc){
    let h='<b>'+pc.events+'</b> playhead events observed';
    if(pc.events===0) h+=' &mdash; play a clip with the step editor visible';
    h+='<table style="margin-top:8px"><tr><th>Track</th><th>Compared</th>'+
       '<th>Agreed</th><th>Last offset</th></tr>';
    for(const t of pc.tracks){
      const pctv = t.seen ? Math.round(100*t.hit/t.seen) : 0;
      /* Offset is in STEPS: 4 = one beat out at 1/16. That is the error that
         matters and the one a count-and-wrap test cannot see. */
      const off = t.seen ? (t.last_diff===0?'0':(t.last_diff>0?'+':'')+t.last_diff+' steps') : '\u2014';
      const cls = !t.seen ? 'off' : (pctv>=95?'ok':(pctv>=5?'warn':'off'));
      h+='<tr><td>'+t.track+'</td><td class="n">'+t.seen+'</td><td>'+
         (t.seen?'<span class="pill '+cls+'">'+pctv+'%</span>':'\u2014')+
         '</td><td class="n">'+off+'</td></tr>';
    }
    document.getElementById('pc').innerHTML=h+'</table>';
  }
  if(d.grid){
    let h='<table class="g"><tr><th></th>';
    for(let s=1;s<=8;s++) h+='<th>'+s+'</th>';
    h+='</tr>';
    for(let t=1;t<=4;t++){
      h+='<tr><th>T'+t+'</th>';
      for(let s=1;s<=8;s++){
        const c=d.grid.find(g=>g.t===t&&g.s===s)||{};
        let cls='', txt='';
        if(c.live){ cls='live'; txt='\u25cf'; }
        else if(c.file_sel){ cls='sel'; txt='\u25cb'; }
        else if(c.exists){ cls='ex'; txt='\u00b7'; }
        h+='<td class="'+cls+'" title="'+(c.len?('loop '+c.len+' beats'):'')+'">'+txt+'</td>';
      }
      h+='</tr>';
    }
    document.getElementById('grid').innerHTML=h+'</table>';
  }
}
tick(); setInterval(tick,1000);
</script></body></html>`
