#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The knob card's header band vs. the screen reader: ONE VALUE, TWO QUESTIONS.
#
# Reported from the device: "the header in the overlay probably doesnt need to
# show the plugin and definitely not the slot. you know where you are. but
# MFX: cloudseed mix doesnt fit for instance, that's not useful."
#
# The band is 116px of content shared with the value, and drawCardHeader
# truncates the NAME on a collision (never the value -- a truncated value is a
# wrong reading), so a composed "MFX: cloudseed Mix" was chewed down to a few
# letters of "MFX: clou" while the diagram BEHIND the card already showed which
# chain and which module were selected.
#
# But announceParameter() had been getting that same composed string, and a
# screen-reader user has no diagram behind the card -- "MFX cloudseed Mix" is
# the only context they get, and "Mix" alone is useless. So the two came apart:
#
#   showKnobFeedback(knobIndex, name, value, raw, cardName)
#                               ^spoken               ^drawn
#
# That split is exactly the kind of thing a later "simplify" collapses back
# into one string, silently taking one of the two users' context away. So it is
# pinned three ways: behaviourally (the card and the announcement disagree, on
# purpose), structurally (every call site that passes a composed ctx.title also
# passes a cardName), and non-vacuously (the derivation is proved to still find
# the call sites at all).
#
# The card state machine is LIFTED out of shadow_ui.js with an explicit
# dependency list -- the same trick tests/host/test_chain_edit_read_budget.sh
# uses -- because shadow_ui.js is 18k lines of device UI that cannot be
# imported. Any free identifier in the lifted region is a ReferenceError here,
# which is the point: it is a real execution, not a regex.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob card header test" >&2
  exit 1
fi

node -e '
Promise.all([
  import("node:fs"),
]).then(([fs]) => {
  let failures = 0;
  const fail = (m) => { console.log("FAIL: " + m); failures++; };

  const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");

  /* ---- lift the card state machine ------------------------------------- */

  const FROM = "const KNOB_CARD_DECAY_MS";
  const TO   = "function showKnobFeedback(";
  const from = src.indexOf(FROM);
  const to   = src.indexOf(TO);
  if (from < 0 || to < 0 || to < from) {
    fail("could not locate the knob card block in shadow_ui.js");
    process.exit(1);
  }
  /* showKnobFeedback ends at the first column-0 closing brace after it. */
  const end = src.indexOf("\n}\n", to);
  if (end < 0) { fail("could not find the end of showKnobFeedback"); process.exit(1); }
  const block = src.slice(from, end + 2);

  /* Everything the block references and does not declare. A name missing here
     throws; a name that stops being needed is harmless. */
  const DEPS = [
    "NUM_KNOBS", "chainEditorFocus", "showOverlay", "hideOverlay", "debugLog",
    "announceParameter", "chainTargetIsModulePosition", "chainTargetHierarchy",
    "chainTargetChainParams", "getKnobContext", "buildMetaIndex", "resolveViz",
    "getSlotParam",
    /* The card hands the renderer its trigger fire times; without this the
       lift throws. A stub returning {} is right here -- this file is about the
       header NAME, and the animation is pinned in its own test. */
    "triggerFiredAtForRow",
  ];

  const spoken = [];
  const overlays = [];
  /* One populated module position in one chain, so knobCardOpen takes its full
     path (hierarchy + chain_params + a row of four reads) rather than the
     short header-only card. The chain is the MASTER bus, because that is the
     one the report was about. */
  const TARGET = {
    kind: "master", slot: 0, label: "MFX",
    key: (comp, suffix) => (comp ? "master_fx:" + comp + ":" + suffix : null),
  };
  const COMP = { key: "fx1", label: "FX 1" };
  const HIER = { levels: { root: { label: "CloudSeed",
                                   knobs: ["mix", "size", "damp", "decay"] } } };
  const PARAMS = [
    { key: "mix",   name: "Mix",   type: "float", min: 0, max: 1, step: 0.01 },
    { key: "size",  name: "Size",  type: "float", min: 0, max: 1, step: 0.01 },
    { key: "damp",  name: "Damp",  type: "float", min: 0, max: 1, step: 0.01 },
    { key: "decay", name: "Decay", type: "float", min: 0, max: 1, step: 0.01 },
  ];
  const KNOB_CTX = PARAMS.map((p, i) => ({
    key: p.key,
    /* What buildChainKnobContext produces for a mapped knob on Master FX. */
    title: "MFX: cloudseed " + p.name,
    cardName: p.name,
  }));

  const env = {
    NUM_KNOBS: 8,
    chainEditorFocus: () => ({ target: TARGET, comp: COMP }),
    showOverlay: (n, v) => overlays.push([n, v]),
    hideOverlay: () => {},
    debugLog: () => {},
    announceParameter: (n, v) => spoken.push(n + " " + v),
    chainTargetIsModulePosition: () => true,
    chainTargetHierarchy: () => HIER,
    chainTargetChainParams: () => PARAMS,
    getKnobContext: (i) => KNOB_CTX[i] || null,
    buildMetaIndex: () => ({}),
    resolveViz: () => ({ groups: null }),
    getSlotParam: () => "0.50",
    triggerFiredAtForRow: () => ({}),
  };

  let api;
  try {
    api = new Function(...DEPS,
      block + "\nreturn { showKnobFeedback, knobCardDrawState, knobCardClose };"
    )(...DEPS.map((d) => env[d]));
  } catch (e) {
    fail("the knob card block does not lift: " + e);
    process.exit(1);
  }

  /* ---- 1. NON-VACUITY: the lift really draws the full card -------------- */
  api.showKnobFeedback(0, "MFX: cloudseed Mix", "0.62", 0.62, "Mix");
  let card = api.knobCardDrawState();
  if (!card) { fail("no card after showKnobFeedback -- the lift is inert"); process.exit(1); }
  if (!card.page || !card.page.keys || !card.page.keys.some(Boolean))
    fail("the lifted card has no widget strip, so it is not the shape the report was about");

  /* ---- 2. THE SPLIT ----------------------------------------------------- */
  if (card.name !== "Mix")
    fail("the card header shows " + JSON.stringify(card.name) +
         ", must be the bare parameter name \"Mix\" -- the chain and the module " +
         "are already on the diagram behind the card");
  if (spoken.length !== 1 || spoken[0] !== "MFX: cloudseed Mix 0.62")
    fail("the announcement lost its context: " + JSON.stringify(spoken) +
         " -- a screen-reader user has no diagram behind the card");
  if (card.value !== "0.62") fail("the card lost its value: " + card.value);

  /* And they must genuinely DIFFER, or the split is decorative. */
  if (card.name === "MFX: cloudseed Mix")
    fail("card header and announcement are the same string again");

  /* ---- 3. the tilde survives -------------------------------------------- */
  /* showKnobOverlay appends "~" to the ANNOUNCED title when the param is
     modulated, and knobCardModKey is derived from that suffix. Moving the
     header to the short name must not move the signal: the tilde is still on
     `name`, the card marks the CELL rather than printing a character. */
  api.knobCardClose();
  spoken.length = 0;
  api.showKnobFeedback(0, "MFX: cloudseed Mix~", "0.62", 0.62, "Mix");
  card = api.knobCardDrawState();
  if (!card.modulated || !card.modulated("mix"))
    fail("the modulation tilde no longer reaches knobCardModKey");
  if (/~/.test(String(card.name)))
    fail("the tilde leaked into the drawn header: " + card.name);
  if (!/Mix~/.test(spoken[0] || ""))
    fail("the tilde was dropped from the announcement: " + JSON.stringify(spoken));

  /* ---- 4. a caller with no separate short name still draws -------------- */
  api.knobCardClose();
  api.showKnobFeedback(1, "Zoom", "4x", undefined);
  card = api.knobCardDrawState();
  if (card.name !== "Zoom")
    fail("omitting cardName must fall back to the announced name, got " +
         JSON.stringify(card.name));

  /* ---- 5. STRUCTURAL: no call site passes a composed title alone -------- */
  /* Every showKnobFeedback(...) whose NAME argument is `ctx.title` is passing
     the composed "<chain>: <module> <param>" form, so it must also hand over a
     cardName -- otherwise that one gesture silently goes back to the long
     header while every other one is short, which is worse than either. */
  const calls = [];
  const re = /showKnobFeedback\s*\(/g;
  let m;
  while ((m = re.exec(src)) !== null) {
    /* Balance parens from the opening one so multi-line calls are captured
       whole; string literals here contain none, so a plain counter is enough. */
    let depth = 0, i = m.index + m[0].length - 1;
    for (; i < src.length; i++) {
      if (src[i] === "(") depth++;
      else if (src[i] === ")") { depth--; if (depth === 0) break; }
    }
    calls.push(src.slice(m.index, i + 1));
  }
  /* The declaration itself is not a call. */
  const sites = calls.filter((c) => !/^showKnobFeedback\s*\(\s*knobIndex,\s*name,/.test(c));
  if (sites.length < 12)
    fail("only " + sites.length + " showKnobFeedback call sites found -- the " +
         "derivation is blind, so the check below proves nothing");
  const composed = sites.filter((c) => /\bctx\.title\b/.test(c));
  if (composed.length < 10)
    fail("only " + composed.length + " call sites pass a composed ctx.title -- " +
         "the derivation is blind");
  const bare = composed.filter((c) => !/\bctx\.cardName\b/.test(c));
  if (bare.length)
    fail(bare.length + " showKnobFeedback call site(s) pass the composed ctx.title " +
         "with no cardName, so the card header would show the long form there: " +
         bare.map((c) => c.replace(/\s+/g, " ").slice(0, 90)).join(" | "));

  /* And the context builder must actually carry the field the sites read --
     checked inside buildChainKnobContext, and in BOTH of the shapes it
     returns. A file-wide grep for "cardName:" passes on the other builders
     alone, so dropping it from the mapped-knob shape (the only one a real
     parameter goes through) would go unnoticed. */
  const bStart = src.indexOf("function buildChainKnobContext(");
  const bEnd = src.indexOf("\n}\n", bStart);
  if (bStart < 0 || bEnd < 0) fail("buildChainKnobContext is gone");
  const builder = src.slice(bStart, bEnd);
  const generic = builder.slice(builder.indexOf("const generic"), builder.indexOf("const mapped"));
  const mapped = builder.slice(builder.indexOf("const mapped"));
  if (!/\bcardName:/.test(generic))
    fail("the no-module / no-mapping knob context no longer declares cardName");
  if (!/\bcardName:\s*displayName\b/.test(mapped))
    fail("the MAPPED knob context no longer declares cardName: displayName -- " +
         "every real parameter would fall back to the composed title");

  if (failures) process.exit(1);
  console.log("PASS: knob card header — the band shows the bare parameter name, the " +
              "announcement keeps the chain and module, the tilde still marks the cell, " +
              "and all " + composed.length + " composed call sites hand over both");
});
'
