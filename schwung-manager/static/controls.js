/*
 * controls.js -- the web editor for the active set's control document
 * (Custom surface pages + CC map). See controls.go.
 *
 * The document is parsed, edited and serialised by the DEVICE'S OWN modules
 * (served from its shared/ directory), so a page written here is exactly what
 * the shadow UI will load. Every edit saves at once, carrying the ETag of the
 * text it was based on; a 409 means the device changed it first, and the page
 * reloads rather than overwrite.
 */
import { parseControls, serializeControls, addPage, renamePage, deletePage, movePage,
         assignKnob, clearKnob, bindCC, unbindCC, setCCMode, MAX_PAGES, KNOBS_PER_PAGE,
         PAGE_NAME_MAX } from "/controls/js/control_map.mjs";
import { componentsOfSlot, paramTargets, settingTargets, scopeText, targetStatus, ccOf }
    from "/controls/js/control_picker.mjs";

const $ = (id) => document.getElementById(id);
const el = (tag, props, ...kids) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(props || {})) {
        if (k === "class") n.className = v;
        else if (k.startsWith("on")) n.addEventListener(k.slice(2), v);
        else if (v !== undefined && v !== null && v !== false) n.setAttribute(k, v === true ? "" : v);
    }
    for (const c of kids.flat()) if (c !== null && c !== undefined) n.append(c);
    return n;
};

const csrf = () => {
    const m = document.cookie.match(/(?:^| )csrf_token=([^;]+)/);
    return m ? m[1] : "";
};

let state = null;   /* { set, etag, doc, chain } */
let saving = Promise.resolve();

function status(text) { $("controls-status").textContent = text; }

async function load(note) {
    try {
        const r = await fetch("/api/controls", { cache: "no-store" });
        const j = await r.json();
        if (!r.ok) { status(j.error || "Could not read the set."); return; }
        const parsed = parseControls(j.text);
        state = { set: j.set, etag: j.etag, doc: parsed.doc, chain: j.chain, broken: !parsed.ok };
        render();
        const where = "Set: " + (j.set.name || j.set.uuid);
        if (parsed.ok === false) status(where + " — controls.json could not be read; it will not be overwritten.");
        else status(where + (j.chain.live ? "" : " — device not answering; modules shown as last saved.") + (note ? " — " + note : ""));
    } catch (e) {
        status("Could not reach the device.");
    }
}

/* Apply one edit and save it. Edits are serialised: one PUT at a time. */
function edit(fn) {
    if (!state || state.broken) return;
    const next = fn(state.doc);
    if (next === state.doc) return;
    state.doc = next;
    render();
    saving = saving.then(save);
}

async function save() {
    status("Saving…");
    try {
        const r = await fetch("/api/controls", {
            method: "PUT",
            headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf() },
            body: JSON.stringify({ uuid: state.set.uuid, etag: state.etag, text: serializeControls(state.doc) }),
        });
        const j = await r.json().catch(() => ({}));
        if (r.status === 409) { await load("it changed on the device, reloaded; please redo your last edit"); return; }
        if (!r.ok) { status("Not saved: " + (j.error || r.status)); return; }
        state.etag = j.etag;
        status("Set: " + (state.set.name || state.set.uuid) + " — saved");
    } catch (e) {
        status("Not saved: could not reach the device.");
    }
}

/* ---------------- the picker ---------------- */

let pickerResolve = null;
let pickerItems = [];

function pickerWheres() {
    const out = [];
    const slots = (state.chain && state.chain.slots) || [];
    for (let s = 0; s < 4; s++) {
        const comps = componentsOfSlot(slots[s]);
        if (comps.length) out.push({ value: "slot" + s, text: "Slot " + (s + 1), comps: comps.map((c) => Object.assign({ slot: s }, c)) });
    }
    const mfx = ((state.chain && state.chain.masterFx) || [])
        .map((m, i) => (m ? { fx: i + 1, module: m, title: "Master FX " + (i + 1) } : null)).filter(Boolean);
    if (mfx.length) out.push({ value: "mfx", text: "Master FX", comps: mfx });
    out.push({ value: "mixer", text: "Mixer", comps: [0, 1, 2, 3].map((s) => ({ settings: s, title: "Slot " + (s + 1) }))
        .concat([{ settings: null, title: "Master" }]) });
    return out;
}

function pickerFill() {
    const wheres = pickerWheres();
    const w = wheres.find((x) => x.value === $("picker-where").value) || wheres[0];
    const comp = $("picker-comp");
    $("picker-comp-label").textContent = w.value === "mixer" ? "Mixer of" : w.value === "mfx" ? "Position" : "Module";
    const prev = comp.value;
    comp.replaceChildren(...w.comps.map((c, i) => el("option", { value: String(i) },
        c.title + (c.module ? " — " + c.module : ""))));
    if ([...comp.options].some((o) => o.value === prev)) comp.value = prev;
    pickerLoadList(w.comps[Number(comp.value) || 0]);
}

async function pickerLoadList(c) {
    const list = $("picker-list");
    list.replaceChildren();
    pickerItems = [];
    if (!c) return;
    if ("settings" in c) {
        pickerItems = settingTargets(c.settings);
        $("picker-note").textContent = "";
        pickerRender();
        return;
    }
    $("picker-note").textContent = "Asking " + c.module + " for its parameters…";
    const q = typeof c.fx === "number" ? "fx=" + c.fx : "slot=" + c.slot + "&comp=" + encodeURIComponent(c.component);
    try {
        const r = await fetch("/api/controls/params?" + q, { cache: "no-store" });
        const j = await r.json();
        if (!r.ok) { $("picker-note").textContent = j.error || "No answer."; return; }
        pickerItems = paramTargets(j, typeof c.fx === "number" ? { fx: c.fx, module: c.module }
                                                              : { slot: c.slot, component: c.component, module: c.module });
        $("picker-note").textContent = pickerItems.length ? "" : c.module + " publishes no parameters.";
        pickerRender();
    } catch (e) {
        $("picker-note").textContent = "Could not reach the device.";
    }
}

function pickerRender() {
    const q = $("picker-search").value.trim().toLowerCase();
    const shown = pickerItems.filter((it) => !q || it.name.toLowerCase().includes(q) ||
        it.target.key.toLowerCase().includes(q) || (it.target.label || "").toLowerCase().includes(q));
    $("picker-list").replaceChildren(...shown.map((it) => {
        const bound = ccOf(state.doc, it.target);
        return el("li", {}, el("button", { type: "button", onclick: () => pickerDone(it.target) },
            el("span", {}, it.name), el("span", { class: "k" }, bound ? bound : it.target.key)));
    }));
}

function pickerDone(result) {
    const d = $("picker");
    if (d.open) d.close();
    const r = pickerResolve;
    pickerResolve = null;
    if (r) r(result);
}

/* Resolves to a target, "clear", or null (cancelled). */
function pick({ allowClear }) {
    /* style, not `hidden`: .btn sets display, which beats the attribute. */
    $("picker-clear").style.display = allowClear ? "" : "none";
    const wheres = pickerWheres();
    const sel = $("picker-where");
    const prev = sel.value;
    sel.replaceChildren(...wheres.map((w) => el("option", { value: w.value }, w.text)));
    if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
    $("picker-search").value = "";
    pickerFill();
    $("picker").showModal();
    $("picker-search").focus();
    return new Promise((res) => { pickerResolve = res; });
}

$("picker-where").addEventListener("change", pickerFill);
$("picker-comp").addEventListener("change", () => {
    const w = pickerWheres().find((x) => x.value === $("picker-where").value);
    pickerLoadList(w && w.comps[Number($("picker-comp").value) || 0]);
});
$("picker-search").addEventListener("input", pickerRender);
$("picker-cancel").addEventListener("click", () => pickerDone(null));
$("picker-clear").addEventListener("click", () => pickerDone("clear"));
$("picker").addEventListener("cancel", () => pickerDone(null));

/* ---------------- pages ---------------- */

function knobCell(p, k, t) {
    const st = t ? targetStatus(t, state.chain) : "";
    const name = t ? (t.label || t.key) : "Empty";
    const where = t ? scopeText(t) + (st === "dark" ? " — not loaded" : "") : "";
    return el("button", {
        type: "button",
        class: "controls-knob" + (t ? "" : " empty") + (st === "dark" ? " dark" : ""),
        "aria-label": "Knob " + (k + 1) + ": " + (t ? name + ", " + where : "empty") + ". Choose a parameter.",
        onclick: async () => {
            const r = await pick({ allowClear: !!t });
            if (r === "clear") edit((d) => clearKnob(d, p, k));
            else if (r) edit((d) => assignKnob(d, p, k, r));
        },
    }, el("span", { class: "n" }, "Knob " + (k + 1)), el("span", { class: "l" }, name), el("span", { class: "s" }, where));
}

function renderPages() {
    const pages = state.doc.surface.pages;
    const host = $("pages");
    if (!pages.length) {
        host.replaceChildren(el("p", { class: "muted" }, "No pages yet."));
    } else {
        host.replaceChildren(...pages.map((pg, p) => {
            let confirmDelete = false;
            const del = el("button", { type: "button", class: "btn btn-small btn-danger" }, "Delete");
            del.addEventListener("click", () => {
                if (!confirmDelete) { confirmDelete = true; del.textContent = "Click again to delete"; return; }
                edit((d) => deletePage(d, p));
            });
            del.addEventListener("blur", () => { confirmDelete = false; del.textContent = "Delete"; });
            const name = el("input", { type: "text", value: pg.name, maxlength: String(PAGE_NAME_MAX),
                                       "aria-label": "Page " + (p + 1) + " name" });
            name.addEventListener("change", () => edit((d) => renamePage(d, p, name.value)));
            return el("section", { class: "controls-page", "aria-label": "Page " + (p + 1) + ": " + pg.name },
                el("div", { class: "controls-page-head" },
                    el("strong", {}, String(p + 1)), name,
                    el("button", { type: "button", class: "btn btn-small btn-secondary", disabled: p === 0,
                                   "aria-label": "Move page earlier", onclick: () => edit((d) => movePage(d, p, p - 1)) }, "↑"),
                    el("button", { type: "button", class: "btn btn-small btn-secondary", disabled: p === pages.length - 1,
                                   "aria-label": "Move page later", onclick: () => edit((d) => movePage(d, p, p + 1)) }, "↓"),
                    del),
                el("div", { class: "controls-grid" },
                    Array.from({ length: KNOBS_PER_PAGE }, (_, k) => knobCell(p, k, pg.knobs[k]))));
        }));
    }
    $("add-page").disabled = pages.length >= MAX_PAGES;
}

$("add-page").addEventListener("click", () => edit((d) => addPage(d)));

/* ---------------- CC map ---------------- */

function renderCC() {
    const tb = $("cc-table").querySelector("tbody");
    const cc = state.doc.cc || [];
    if (!cc.length) {
        tb.replaceChildren(el("tr", {}, el("td", { colspan: "5", class: "muted" }, "No bindings yet.")));
        return;
    }
    tb.replaceChildren(...cc.map((b, i) => {
        const st = targetStatus(b.target, state.chain);
        const mode = el("select", { "aria-label": "Mode for channel " + (b.channel + 1) + " CC " + b.cc },
            el("option", { value: "abs", selected: b.mode === "abs" }, "Absolute"),
            el("option", { value: "rel", selected: b.mode === "rel" }, "Relative"));
        mode.addEventListener("change", () => edit((d) => setCCMode(d, i, mode.value)));
        const target = el("button", { type: "button", class: "btn-link" + (st === "dark" ? " controls-dark" : ""),
            onclick: async () => {
                const r = await pick({ allowClear: false });
                if (r && r !== "clear") edit((d) => bindCC(d, { channel: b.channel, cc: b.cc, mode: b.mode, target: r }));
            } }, (b.target.label || b.target.key));
        return el("tr", {},
            el("td", {}, String(b.channel + 1)),
            el("td", {}, String(b.cc)),
            el("td", {}, mode),
            el("td", {}, target, el("div", { class: "muted" }, scopeText(b.target) + (st === "dark" ? " — not loaded" : ""))),
            el("td", {}, el("button", { type: "button", class: "btn btn-small btn-danger",
                "aria-label": "Remove channel " + (b.channel + 1) + " CC " + b.cc,
                onclick: () => edit((d) => unbindCC(d, i)) }, "Remove")));
    }));
}

$("cc-add-ch").replaceChildren(...Array.from({ length: 16 }, (_, i) => el("option", { value: String(i) }, String(i + 1))));
$("cc-add").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const ch = Number($("cc-add-ch").value), cc = Number($("cc-add-cc").value), mode = $("cc-add-mode").value;
    if (!Number.isInteger(cc) || cc < 0 || cc > 127) return;
    const r = await pick({ allowClear: false });
    if (r && r !== "clear") edit((d) => bindCC(d, { channel: ch, cc, mode, target: r }));
});

function render() {
    renderPages();
    renderCC();
}

/* The device may change the document too (learn); pick that up when the
 * page comes back into view rather than polling. */
document.addEventListener("visibilitychange", () => { if (!document.hidden && !$("picker").open) load(); });

load();
