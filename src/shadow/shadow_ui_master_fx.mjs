/*
 * Shadow UI - Master FX views (chain, settings, module select, presets).
 *
 * Extracted from shadow_ui.js to allow forks to modify Master FX
 * presentation without touching core.
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    SCREEN_WIDTH,
    /* No LIST_LINE_HEIGHT / LIST_HIGHLIGHT_HEIGHT / LIST_LABEL_X /
     * LIST_VALUE_X: nothing here draws a row any more. */
    LIST_TOP_Y,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawChainDiagram, DIAGRAM_W, DEFAULT_Y as DIAGRAM_Y
} from '/data/UserData/schwung/shared/chain_diagram.mjs';
/* The bands around the row of boxes — header, label, info, footer. The SAME
 * call drawChainEdit makes: 4a-3 of the Master FX variable-length design. */
import { drawChainEditorBands, drawChainPicker, shiftHintsFor, CHAIN_HINTS_AT_REST }
    from '/data/UserData/schwung/shared/chain_editor_chrome.mjs';
/* The knob card, drawn over the diagram — the SAME renderer drawChainEdit uses.
 * It shipped 2026-08-20 scoped to the slot chain, so a Master FX knob still
 * raised the old centred `Value: 0.62` box; step 4b of the Master FX
 * variable-length design is that scope boundary being repaid. */
import { drawKnobCard }
    from '/data/UserData/schwung/shared/param_pages/knob_card.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList,
    drawConfirmModal,
    drawNamePreview
} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {
    announce, announceMenuItem
} from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ---- Enter -------------------------------------------------------------- */

export function enterMasterFxSettings() {
    const { scanForAudioFxModules, loadMasterFxChainConfig,
            getMasterFxSlotModule, setView, VIEWS } = ctx;

    ctx.MASTER_FX_OPTIONS = scanForAudioFxModules();
    loadMasterFxChainConfig();
    ctx.selectedMasterFxComponent = 0;
    ctx.selectingMasterFxModule = false;
    setView(VIEWS.MASTER_FX);
    ctx.needsRedraw = true;

    /* READ AFTER the load: the component list is derived from the chain, so it
     * is only as long as what was just loaded. On an empty chain position 0 is
     * the `+`, whose label already is the whole instruction — "Add FX, Empty"
     * would say nothing. */
    const comp = ctx.MASTER_FX_CHAIN_COMPONENTS[0];
    if (!comp) announce("Master FX");
    else if (comp.kind === "add") announce(`Master FX, ${comp.label}`);
    else announce(`Master FX, ${comp.label} ${getMasterFxSlotModule(0) || "Empty"}`);
}

/* ---- Display name (used in slot list) ----------------------------------- */

export function getMasterFxDisplayName() {
    /* MASTER_FX_SLOTS comes across on ctx, mirroring the constant in
     * shadow_ui.js (itself a mirror of shadow_chain_mgmt.h). Never re-derive
     * the cap here. */
    const { masterFxConfig, MASTER_FX_SLOTS } = ctx;
    const parts = [];
    for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
        const key = `fx${i}`;
        if (masterFxConfig[key]?.module) {
            parts.push(masterFxConfig[key].module);
        }
    }
    return parts.length > 0 ? parts.join("+") : "None";
}

/* ---- Draw --------------------------------------------------------------- */

export function drawMasterFx() {
    const { masterShowingNamePreview, masterConfirmingOverwrite,
            masterConfirmingDelete, helpDetailScrollState, helpNavStack,
            inMasterPresetPicker, inMasterFxSettingsMenu,
            selectingMasterFxModule, selectedMasterFxComponent,
            masterFxConfig, MASTER_FX_OPTIONS,
            currentMasterPresetName, getMasterFxParam,
            getModuleAbbrev, isTextEntryActive, drawTextEntry,
            drawHelpDetail, drawHelpList,
            MASTER_CHAIN_TARGET, chainLfoTargetMap, chainComponentBypassed,
            ensureMasterFxConfigFresh, isShiftHeld,
            knobCardDrawState } = ctx;

    clear_screen();

    if (isTextEntryActive()) {
        drawTextEntry();
        return;
    }

    if (masterShowingNamePreview) {
        drawMasterNamePreview();
        return;
    }

    if (masterConfirmingOverwrite) {
        drawMasterConfirmOverwrite();
        return;
    }

    if (masterConfirmingDelete) {
        drawMasterConfirmDelete();
        return;
    }

    if (helpDetailScrollState) {
        drawHelpDetail();
        return;
    }
    if (helpNavStack.length > 0) {
        drawHelpList();
        return;
    }

    if (inMasterPresetPicker) {
        drawMasterPresetPicker();
        return;
    }

    if (inMasterFxSettingsMenu) {
        drawMasterFxSettingsMenu();
        return;
    }

    if (selectingMasterFxModule) {
        drawMasterFxModuleSelect();
        return;
    }

    /*
     * The row of boxes is shared/chain_diagram.mjs — the same renderer the slot
     * chain editor draws, so the two screens stop looking like different
     * products.
     *
     * What it replaces was a fixed row: `TOTAL_W = 5 * BOX_W + 4 * GAP`, five
     * 22px boxes filling 118 of the 128px screen exactly. A fixed row cannot
     * report that it overflowed. At the 8-slot cap the nine boxes are 214px
     * wide, and boxes 6..9 — together with their bypass "B" and LFO "~"
     * markers, which were drawn by two more hand-rolled loops over the FULL
     * component list — would have gone off the right edge with no clipping and
     * no error. The shared diagram windows the row around the selection
     * (scrollWindow in chain_model.mjs) and puts a dotted rail in the margin on
     * whichever side has more chain, so the count can grow without the layout
     * having an opinion about it.
     */
    /* The slot editor's own box row, to the row. This was 20 — six rows lower —
     * because Master FX still wore the taller menu_layout header when the slot
     * editor moved to the movy band; nothing chose the gap. DIAGRAM_Y is the
     * diagram's own default, so neither screen restates it. */
    /*
     * The chain, reloaded from the DSP only when something has made it stale —
     * a shape edit renumbers the positions underneath the editor without
     * changing WHICH modules it holds, so nothing keyed on a module id would
     * notice. Here rather than higher up because the `+` box materialises a
     * position in the MODEL ONLY, and the picker draws through one of the early
     * returns above: reloading before them would wipe the pending position out
     * from under the picker that was just opened on it.
     */
    ensureMasterFxConfigFresh();
    /* AFTER the reload, and through the ctx getter, so the list is as long as
     * the chain actually is. */
    const MASTER_FX_CHAIN_COMPONENTS = ctx.MASTER_FX_CHAIN_COMPONENTS;

    const BOX_Y = DIAGRAM_Y;
    /* Centred, unlike the slot editor, which shifts right to clear its
     * slot-indicator column. THIS difference is real and stays: Master FX is
     * one chain, not one of four, so there is no indicator column to clear and
     * an 8px offset would just push the strip off-centre for nothing. */
    const START_X = Math.floor((SCREEN_WIDTH - DIAGRAM_W) / 2);

    const presetSelected = selectedMasterFxComponent === -1;

    /* The device hands the renderers these four primitives and nothing else. */
    const dctx = {
        fillRect: fill_rect,
        print: print,
        textWidth: typeof text_width === "function" ? text_width : undefined,
        setPixel: set_pixel,
    };

    /*
     * Which components an LFO is pointed at. Four IPC reads, FIXED — the
     * question is asked of the two LFOs, never of each box, so it does not grow
     * with the slot cap.
     *
     * This is chainLfoTargetMap, the SAME function the slot chain editor calls,
     * aimed at the master target. Two copies of it is how the two screens end
     * up disagreeing about what an LFO marker means.
     */
    const mfxLfoTargets = chainLfoTargetMap(MASTER_CHAIN_TARGET);

    drawChainDiagram(dctx, MASTER_FX_CHAIN_COMPONENTS, selectedMasterFxComponent, {
        x: START_X,
        y: BOX_Y,
        /* Selecting the preset row lights the whole chain, as it always has. */
        allSelected: presetSelected,
        abbrev: (comp) => {
            if (comp.kind === "add") return "+";
            if (comp.kind === "settings") return "*";
            /* masterFxConfig is the cached copy the editor already holds — no
             * IPC here. Nothing in this list is `kind: "synth"`, so no box gets
             * the synth band: Master FX has no synth to landmark. */
            const moduleData = masterFxConfig[comp.key];
            return (moduleData && moduleData.module) ? getModuleAbbrev(moduleData.module) : "--";
        },
        marks: (comp) => {
            /* Neither the settings box nor the `+` is an FX position: neither
             * has a bypass parameter and neither can be an LFO target, so
             * neither must ever be asked. */
            if (comp.kind !== "module") return null;
            const lfo = mfxLfoTargets[comp.key];
            /* One read per DRAWN box — at most the diagram capacity, five —
             * rather than one per slot. The loop this came from ran the whole
             * component list, so the 4 -> 8 raise would otherwise have doubled
             * the per-frame IPC cost of a screen that redraws every frame, at
             * ~2.8ms a read against a 1.68ms whole-page render. */
            const bypassed = chainComponentBypassed(MASTER_CHAIN_TARGET, comp.key);
            if (!lfo && !bypassed) return null;
            return { bypassed, lfo1: lfo && lfo.lfo1, lfo2: lfo && lfo.lfo2 };
        },
    });

    const selectedComp = presetSelected ? null : MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
    /* The label and info bands are drawn by drawChainEditorBands below, at the
     * slot editor's spacing (+3 / +11). They were +4 / +12 here, which is the
     * same kind of accident as the box row: nobody chose a Master-FX-specific
     * gutter, the two screens were just written at different times. */
    const label = presetSelected ? "Preset" : (selectedComp ? selectedComp.label : "");

    let infoLine = "";
    if (presetSelected) {
        /* The PRESET ROW, at index -1, is Master FX's alone and stays: the slot
         * editor's -1 is the patch and it says "Chain". Both are "the whole
         * thing rather than one position in it", which is why they share the
         * band — they just name different objects. */
        infoLine = currentMasterPresetName || "(no preset)";
    } else if (selectedComp && selectedComp.kind === "add") {
        infoLine = "New effect";
    } else if (selectedComp && selectedComp.kind === "module") {
        const moduleData = masterFxConfig[selectedComp.key];
        if (moduleData && moduleData.module) {
            const opt = MASTER_FX_OPTIONS.find(o => o.id === moduleData.module);
            const displayName = opt ? opt.name : moduleData.module;
            const preset = getMasterFxParam(selectedMasterFxComponent, "preset_name") ||
                          getMasterFxParam(selectedMasterFxComponent, "preset") || "";
            infoLine = preset ? `${displayName} (${truncateText(preset, 8)})` : displayName;
        } else {
            infoLine = "(empty)";
        }
    } else if (selectedComp && selectedComp.kind === "settings") {
        infoLine = "Configure master FX";
    }

    /*
     * The bands, from the same function the slot editor calls.
     *
     * HEADER. Left is the screen's own identity plus the thing that names this
     * chain — the preset — exactly where the slot editor puts its patch name.
     * Right is "MFX", the screen name, which is what the slot editor puts there
     * when it has no synth to landmark ("CHAIN"): Master FX never has one, so
     * there is no honest value for that side and a constant is the truth.
     * What this replaces was drawMenuHeader("Master FX") — the device 5x7 font
     * and a rule, ~18 rows — which is the header every other screen left behind
     * for the movy band.
     *
     * FOOTER. Master FX had none at all, so nothing on the screen said what the
     * jog or the click did. handleJog moves the selection, handleSelect opens
     * the picker / settings / preset list, and Back at this level calls
     * shadow_request_exit — it leaves shadow mode entirely, same as the chain
     * editor, so the word is EXIT and not OUT.
     *
     * The hints follow the modifier, exactly as the slot editor`s do, because
     * Shift silently repurposes the jog into a reorder: a move gesture with a
     * footer still reading SEL is a gesture nobody finds. 4a-3 left this pair
     * out deliberately — the gesture did not exist then. It does now.
     */
    drawChainEditorBands(dctx, {
        headerLeft: currentMasterPresetName || "Master FX",
        headerRight: "MFX",
        label,
        info: infoLine,
        hints: isShiftHeld() ? shiftHintsFor(selectedComp)
                             : CHAIN_HINTS_AT_REST,
    });

    /*
     * The card last, over everything — it is a modal. Every value it draws was
     * read once on touch-down (knobCardOpen in shadow_ui.js), so this costs no
     * IPC per frame, which is the whole design argument for the feature: a
     * round trip is ~2.8ms against a 1.68ms whole-page render.
     *
     * The early returns above are the reason masterFxChainDiagramVisible()
     * exists: this is the ONLY path that draws the card, so the touch handler
     * must not raise one while a picker or a confirm is covering the diagram.
     *
     * knobCardDrawState comes over ctx (it reads shadow_ui.js state); it is
     * destructured at the top rather than being a free identifier because it
     * cannot be both that and a lifted parameter in the tests — a `const`
     * cannot shadow a parameter of the same name. drawKnobCard IS free, so
     * it is in MFX_DRAW_DEPS in both tests that lift this function.
     */
    const card = knobCardDrawState();
    if (card) {
        /* dctx carries the four primitives the diagram needs. The card draws
         * real widgets — arc knobs, enum squares, bars — and each of those
         * probes for a native primitive and takes a slow JS path without it.
         * A SEPARATE object rather than four more fields on dctx: the renderers
         * branch on what they are handed, so widening dctx could move diagram
         * pixels, and the whole of 4a was about the two screens rendering the
         * same. This is the same probe list drawChainEdit builds. */
        drawKnobCard({
            fillRect: fill_rect, print, textWidth: text_width, setPixel: set_pixel,
            line: typeof draw_line === "function" ? draw_line : undefined,
            fillCircle: typeof fill_circle === "function" ? fill_circle : undefined,
            drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
            drawArc: typeof draw_arc === "function" ? draw_arc : undefined,
        }, card);
    }
}

/*
 * STAYS ON THE OLD MENU CHROME, deliberately, and this is the reasoning so the
 * next person does not have to redo it.
 *
 * Its twin is the slot chain's settings screen, drawChainSettings in
 * shadow_ui_settings.mjs, and that one is on the SAME old chrome —
 * drawMenuHeader, a list, a text footer. The two agree today. Converting this
 * one alone would recreate the exact bug that brought us here, only mirrored:
 * two settings screens, one movy and one not. Whoever moves this moves both, in
 * one change, and reads the pair side by side afterwards.
 */
function drawMasterFxSettingsMenu() {
    const { currentMasterPresetName, selectedMasterFxSetting,
            getMasterFxSettingsItems, getMasterFxSettingValue } = ctx;

    const title = currentMasterPresetName || "Master FX";
    drawHeader(truncateText(title, 18));

    const items = getMasterFxSettingsItems();

    drawMenuList({
        items: items,
        selectedIndex: selectedMasterFxSetting,
        getLabel: (item) => item.label,
        getValue: (item) => {
            if (item.type === "action") return "";
            return getMasterFxSettingValue(item);
        },
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true
    });

    drawFooter("Back: FX chain");
}

/*
 * The module picker, drawn by drawChainPicker — the SAME function the slot
 * chain's drawComponentSelect calls.
 *
 * What this replaces, and why it had to go: the two pickers already chose from
 * the same rows (the module scan plus this position's Move Left / Move Right,
 * both built by chainMoveEntries) and then drew them completely differently —
 * this one wore drawMenuHeader's "Select FX 2", drawMenuList, and a
 * "Back: cancel / Click: apply" text footer, while the slot picker had moved to
 * the movy band, renderPicker and the [JOG SEL][CLK LOAD][BACK EXIT] hints.
 * Reported from the device as "the module select here is different than the
 * module select in slots", which is precisely the drift §1b of the
 * variable-length design exists to end.
 *
 * The `*` on the loaded module, the Move rows, the empty-list message and where
 * Back goes are all unchanged: Back cancels the selection and returns to the
 * Master FX chain view (handleBack, VIEWS.MASTER_FX), which is EXIT by
 * FOOTER_CANON — it leaves the picker entirely, the same as the slot picker's.
 */
function drawMasterFxModuleSelect() {
    const { selectedMasterFxComponent, MASTER_FX_CHAIN_COMPONENTS,
            masterFxPickerItems, selectedMasterFxModuleIndex,
            masterFxConfig } = ctx;

    const comp = MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];

    drawChainPicker({
        fillRect: fill_rect, print,
        textWidth: typeof text_width === "function" ? text_width : undefined,
    }, {
        /* The slot picker's header grammar — which chain, then which position
         * in it — with MFX where the slot editor says S1. Same two words the
         * Master FX chain view already puts on the right of its own band. */
        headerLeft: `MFX > ${comp ? comp.label : "FX"}`,
        entries: masterFxPickerItems,
        index: selectedMasterFxModuleIndex,
        currentId: comp ? (masterFxConfig[comp.key]?.module || "") : "",
        /* Master FX loads audio FX and nothing else, so the empty case can say
         * so; the slot picker, which can be opened on a synth or a MIDI FX,
         * cannot. That is a difference in what the two lists CONTAIN, not in
         * how they are drawn. */
        emptyMessage: "No FX modules available",
    });
}

/*
 * Also stays, for the same reason: its twin is the slot chain's patch library
 * (drawPatches in shadow_ui_patches.mjs), which lists saved chains with a `*`
 * on the loaded one and a "Back: settings / Click: load" footer — the same
 * screen, the same chrome. The pair is consistent; moving one of them is what
 * would make it not.
 *
 * (The `[New]` first row is genuinely Master-FX-only — the slot side reaches
 * Save from its settings menu instead — but that is a difference in ROWS, not
 * in how they are drawn, and it is not what anyone noticed.)
 */
function drawMasterPresetPicker() {
    const { masterPresets, selectedMasterPresetIndex,
            currentMasterPresetName } = ctx;

    drawHeader("Master Presets");

    const items = [{ name: "[New]", index: -1 }];
    for (let i = 0; i < masterPresets.length; i++) {
        items.push(masterPresets[i]);
    }

    drawMenuList({
        items: items,
        selectedIndex: selectedMasterPresetIndex,
        getLabel: (item) => {
            const isCurrent = item.index >= 0 && masterPresets[item.index]?.name === currentMasterPresetName;
            return isCurrent ? `* ${item.name}` : item.name;
        },
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y }
    });

    drawFooter("Back: cancel");
}

function drawMasterNamePreview() {
    const { masterPendingSaveName, masterNamePreviewIndex } = ctx;
    drawNamePreview({ name: masterPendingSaveName, selectedIndex: masterNamePreviewIndex });
}

function drawMasterConfirmOverwrite() {
    const { masterPendingSaveName, masterConfirmIndex } = ctx;
    drawConfirmModal({ title: "Overwrite?", name: masterPendingSaveName, selectedIndex: masterConfirmIndex });
}

function drawMasterConfirmDelete() {
    const { currentMasterPresetName, masterConfirmIndex } = ctx;
    drawConfirmModal({ title: "Delete?", name: currentMasterPresetName, selectedIndex: masterConfirmIndex });
}
