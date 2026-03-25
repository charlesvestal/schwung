#!/usr/bin/env bash
set -euo pipefail

shadow_file="src/shadow/shadow_ui.js"
docs_file="docs/MODULES.md"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -F -q "function normalizeExpandedParamMeta(key, meta) {" "$shadow_file"; then
  echo "FAIL: expanded parameter metadata normalizer is missing" >&2
  exit 1
fi

if ! rg -F -q "function buildNoteParamMeta(meta) {" "$shadow_file"; then
  echo "FAIL: note type metadata builder is missing" >&2
  exit 1
fi

if ! rg -F -q "function buildRateParamMeta(meta) {" "$shadow_file"; then
  echo "FAIL: rate type metadata builder is missing" >&2
  exit 1
fi

if ! rg -F -q "bars-simple" "$shadow_file"; then
  echo "FAIL: rate type is missing bars-simple mode support" >&2
  exit 1
fi

if ! rg -F -q "bars-every" "$shadow_file"; then
  echo "FAIL: rate type is missing bars-every mode support" >&2
  exit 1
fi

if ! rg -F -q "pushRate(\"1 bar\")" "$shadow_file"; then
  echo "FAIL: rate type should emit '1 bar' when bars are enabled" >&2
  exit 1
fi

if rg -F -q "pushRate(\"1/1\")" "$shadow_file"; then
  echo "FAIL: rate type should not emit '1/1' as a base division" >&2
  exit 1
fi

if ! rg -F -q "const RATE_BASE_DENOMS = [2, 4, 8, 16, 32, 64];" "$shadow_file"; then
  echo "FAIL: rate base denominators should start at 1/2 (not 1/1)" >&2
  exit 1
fi

if rg -F -q "include_even" "$shadow_file"; then
  echo "FAIL: legacy include_even support should be removed from rate type" >&2
  exit 1
fi

if rg -F -q "include_odd" "$shadow_file"; then
  echo "FAIL: legacy include_odd support should be removed from rate type" >&2
  exit 1
fi

if ! rg -F -q "function getWavPositionPreviewData(fullKey, meta) {" "$shadow_file"; then
  echo "FAIL: wav_position preview helper is missing" >&2
  exit 1
fi

if ! rg -F -q "function drawWavPositionPreview() {" "$shadow_file"; then
  echo "FAIL: wav_position preview renderer is missing" >&2
  exit 1
fi

if ! rg -F -q "function drawWavPositionEditor(selectedKey, selectedMeta) {" "$shadow_file"; then
  echo "FAIL: wav_position edit-mode waveform renderer is missing" >&2
  exit 1
fi

if ! rg -F -q "function drawWavPositionEditor(selectedKey, selectedMeta) {" "$shadow_file" || ! rg -F -q "clear_screen();" "$shadow_file"; then
  echo "FAIL: wav_position editor should clear background before drawing" >&2
  exit 1
fi

if ! rg -F -q "hierEditorEditMode && selectedMeta && selectedMeta.ui_type === \"wav_position\"" "$shadow_file"; then
  echo "FAIL: wav_position waveform should only render while editing" >&2
  exit 1
fi

if ! rg -F -q "if (meta && meta.ui_type === \"wav_position\" && isShiftHeld()) {" "$shadow_file"; then
  echo "FAIL: wav_position jog editing is missing shift fine-step handling" >&2
  exit 1
fi

if ! rg -F -q "function getWavPositionShiftMultiplier(meta) {" "$shadow_file"; then
  echo "FAIL: wav_position shift multiplier helper is missing" >&2
  exit 1
fi

if ! rg -F -q "shift_increment_multiplier" "$shadow_file"; then
  echo "FAIL: wav_position shift increment multiplier metadata is missing" >&2
  exit 1
fi

if ! rg -F -q "const defaultMax = 1;" "$shadow_file"; then
  echo "FAIL: wav_position percent mode should default to normalized 0..1 range" >&2
  exit 1
fi

if ! rg -F -q "const defaultStep = displayUnit === \"ms\" ? 1 : 0.01;" "$shadow_file"; then
  echo "FAIL: wav_position percent/sec default step should be 0.01" >&2
  exit 1
fi

if ! rg -F -q "Math.abs(baseStep) * getWavPositionShiftMultiplier(ctx.meta)" "$shadow_file"; then
  echo "FAIL: wav_position knob shift step should use metadata multiplier" >&2
  exit 1
fi

if ! rg -F -q "modeRaw === \"trim_front\" || modeRaw === \"start\"" "$shadow_file"; then
  echo "FAIL: wav_position start-mode alias mapping is missing" >&2
  exit 1
fi

if ! rg -F -q "modeRaw === \"trim_end\" || modeRaw === \"end\"" "$shadow_file"; then
  echo "FAIL: wav_position end-mode alias mapping is missing" >&2
  exit 1
fi

if ! rg -F -q "function applyLinkedWavEndDefaultsForFilepath(filepathKey) {" "$shadow_file"; then
  echo "FAIL: linked wav end-default sync helper is missing" >&2
  exit 1
fi

if ! rg -F -q "applyLinkedWavEndDefaultsForFilepath(key);" "$shadow_file"; then
  echo "FAIL: filepath selection should sync linked wav end defaults" >&2
  exit 1
fi

if ! rg -F -q "function getWavPositionSourcePathForLevel(meta, levelDef, childIndex) {" "$shadow_file"; then
  echo "FAIL: wav_position filepath resolution helper for cross-level sync is missing" >&2
  exit 1
fi

if ! rg -F -q "Object.values(hierEditorHierarchy.levels)" "$shadow_file"; then
  echo "FAIL: linked wav end-default sync should scan hierarchy levels" >&2
  exit 1
fi

if rg -F -q "if (mode === \"end\" && isEmptyParamValue(value)) {" "$shadow_file"; then
  echo "FAIL: wav end-default should not auto-apply on editor open anymore" >&2
  exit 1
fi

if ! rg -F -q "function evaluateVisibilityCondition(condition, levelDef) {" "$shadow_file"; then
  echo "FAIL: visibility condition evaluator is missing" >&2
  exit 1
fi

if ! rg -F -q "function filterHierarchyParamsByVisibility(levelDef, params) {" "$shadow_file"; then
  echo "FAIL: hierarchy visibility filter is missing" >&2
  exit 1
fi

if ! rg -F -q "meta && meta.type === \"string\"" "$shadow_file"; then
  echo "FAIL: string parameter text-entry handling is missing" >&2
  exit 1
fi

if ! rg -F -q "meta && meta.type === \"canvas\"" "$shadow_file"; then
  echo "FAIL: canvas parameter handling is missing" >&2
  exit 1
fi

if ! rg -F -q "function openCanvasPreview(paramKey, meta) {" "$shadow_file"; then
  echo "FAIL: canvas open-preview handler is missing" >&2
  exit 1
fi

if ! rg -F -q "function drawCanvasPreview() {" "$shadow_file"; then
  echo "FAIL: canvas preview renderer is missing" >&2
  exit 1
fi

if ! rg -F -q "getMetaOption(meta, \"show_footer\", getMetaOption(meta, \"showfooter\", true))" "$shadow_file"; then
  echo "FAIL: canvas show_footer/showfooter metadata parsing is missing" >&2
  exit 1
fi

if ! rg -F -q "getMetaOption(meta, \"show_value\", getMetaOption(meta, \"showvalue\", true))" "$shadow_file"; then
  echo "FAIL: canvas show_value/showvalue metadata parsing is missing" >&2
  exit 1
fi

if ! rg -F -q "if (meta && meta.type === \"canvas\" && meta.show_value === false) {" "$shadow_file"; then
  echo "FAIL: canvas should support hiding parameter value in hierarchy list" >&2
  exit 1
fi

if ! rg -F -q "canvasParamMeta.show_footer !== false" "$shadow_file"; then
  echo "FAIL: canvas footer visibility flag handling is missing" >&2
  exit 1
fi

if ! rg -F -q "const showCanvasValue = !canvasParamMeta || canvasParamMeta.show_value !== false;" "$shadow_file"; then
  echo "FAIL: canvas preview footer should honor show_value flag" >&2
  exit 1
fi

if ! rg -F -q "function dispatchCanvasMidi(data, source) {" "$shadow_file"; then
  echo "FAIL: canvas MIDI dispatch handler is missing" >&2
  exit 1
fi

if ! rg -F -q "setView(VIEWS.CANVAS);" "$shadow_file"; then
  echo "FAIL: canvas parameter should transition into dedicated canvas view" >&2
  exit 1
fi

if ! rg -F -q "| \`note\` | \`mode\`, \`min_note\`, \`max_note\` |" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing note parameter type documentation" >&2
  exit 1
fi

if ! rg -F -q "| \`filepath\` | \`root\`, \`start_path\`, \`filter\` |" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing filepath parameter type documentation" >&2
  exit 1
fi

if ! rg -F -q "| \`module_picker\` | \`allow_none\`, \`allow_self\`, \`allowed_targets\`, \`param_key\` |" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing module_picker parameter type documentation" >&2
  exit 1
fi

if ! rg -F -q "| \`parameter_picker\` | \`target_key\`, \`numeric_only\`, \`allow_none\` |" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing parameter_picker parameter type documentation" >&2
  exit 1
fi

if ! rg -F -q "Use the canonical type list in \`Shadow UI Parameter Hierarchy -> Parameter Types\`." "$docs_file"; then
  echo "FAIL: docs/MODULES.md should point chain_params to the canonical parameter type list" >&2
  exit 1
fi

if ! rg -F -q "| \`wav_position\` | \`display_unit\`, \`mode\`, \`filepath_param\`, \`min\`, \`max\`, \`step\`, \`shift_increment_multiplier\` |" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing wav_position parameter type documentation" >&2
  exit 1
fi

if ! rg -F -q "Waveform view opens only while the parameter is in edit mode" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing wav_position edit-mode waveform behavior" >&2
  exit 1
fi

if ! rg -F -q "In \`percent\` mode values are stored internally as normalized \`0..1\` and displayed as \`0..100%\`." "$docs_file"; then
  echo "FAIL: docs/MODULES.md should document wav_position percent internal value range" >&2
  exit 1
fi

if ! rg -F -q "\`mode\` (optional): \`position\`, \`start\`, \`end\`" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing wav_position mode guidance" >&2
  exit 1
fi

if ! rg -F -q "\`shift_increment_multiplier\` (optional): Multiplier for Shift fine-step (default \`0.1\`; alias \`shift_step_multiplier\`)." "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing wav_position shift multiplier guidance" >&2
  exit 1
fi

if ! rg -F -q "empty linked \`mode: end\` params are initialized to file end" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing wav_position end-mode auto-default behavior" >&2
  exit 1
fi

if ! rg -F -q "| \`rate\` | \`include_bars\`, \`bars_mode\`, \`include_triplets\` |" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing updated rate parameter fields" >&2
  exit 1
fi

if rg -F -q "include_even" "$docs_file"; then
  echo "FAIL: docs/MODULES.md should not mention include_even for rate type" >&2
  exit 1
fi

if rg -F -q "include_odd" "$docs_file"; then
  echo "FAIL: docs/MODULES.md should not mention include_odd for rate type" >&2
  exit 1
fi

if ! rg -F -q "1 bar, 1/1T, 1/2, 1/2T, 1/4" "$docs_file"; then
  echo "FAIL: docs/MODULES.md should describe 1 bar replacing 1/1 in rate ordering" >&2
  exit 1
fi

if ! rg -F -q "\`canvas_script\` (optional): Script path relative to module root (default \`canvas.js\`), supports \`file.js#overlay_name\`." "$docs_file"; then
  echo "FAIL: docs/MODULES.md should describe canvas_script behavior" >&2
  exit 1
fi

if ! rg -F -q "globalThis.canvas_overlay" "$docs_file"; then
  echo "FAIL: docs/MODULES.md should describe canvas overlay exports" >&2
  exit 1
fi

if ! rg -F -q "\`show_footer\` (optional): Show/hide footer in canvas view (default \`true\`; alias \`showfooter\`)." "$docs_file"; then
  echo "FAIL: docs/MODULES.md should describe canvas footer visibility metadata" >&2
  exit 1
fi

if ! rg -F -q "\`show_value\` (optional): Show/hide parameter value in hierarchy and canvas footer (default \`true\`; alias \`showvalue\`)." "$docs_file"; then
  echo "FAIL: docs/MODULES.md should describe canvas show_value metadata" >&2
  exit 1
fi

if ! rg -F -q "Supported condition fields:" "$docs_file"; then
  echo "FAIL: docs/MODULES.md is missing visible_if condition guidance" >&2
  exit 1
fi

echo "PASS: expanded shadow parameter type plumbing present"
exit 0
