import { shouldFilterMessage, decodeDelta, setLED } from '/data/UserData/schwung/shared/input_filter.mjs';

const FONT_SIZES = [9, 12, 13, 14, 15, 16, 20];
const FONT_BASE = "/data/UserData/schwung/host/fonts/tamzen-";
const DEFAULT_FONT = "/data/UserData/schwung/host/font.png";
const CAPTURE_DIR = "/data/UserData/schwung/ui-test-captures";
let captureCount = 0;

const SCREEN_W = 128;
const SCREEN_H = 64;

/* Demo content - mix of plain, key-value, edit, and sublabel items */
const DEMO_ITEMS = [
    { label: "Preset 1", type: "plain" },
    { label: "Cutoff", value: "72%", type: "value" },
    { label: "Resonance", value: "45%", type: "edit" },
    { label: "Warm Keys", type: "plain", subLabel: "Bank A / Slot 3" },
    { label: "Mode", value: "LP", type: "value" },
    { label: "Pluck Arp", type: "plain" },
    { label: "Drive", value: "Off", type: "value" },
    { label: "Sub Bass", type: "plain" },
    { label: "Release", value: "1.2s", type: "edit" },
    { label: "Pad Wash", type: "plain", subLabel: "Ambient texture" },
];

/* ==================== PARAMETERS ==================== */

/* Page 0: Title */
let titleFontIdx = 2;       // 13px
let titleX = 2;
let titleY = 2;
let titleRuleY = 12;
// shift+knobs page 0
let titleRuleThick = 1;
let titleText = 0;          // index into title text options
const TITLE_TEXTS = ["Header Text", "Settings", "Sound Generator", "Patches", "FX Chain"];

/* Page 1: List */
let listFontIdx = 0;        // 9px
let listTopY = 15;
let listLineH = 9;
let listMaxVis = 5;
let listLabelX = 4;
// shift+knobs page 1
let selectedItem = 2;       // default to edit item
let listScrollStart = 0;

/* Page 2: Highlight & Prefix */
let hlHeight = 9;
let hlYOff = 1;
let hlPad = 0;              // extra pad added to hlHeight
let hlWidth = 128;          // highlight rect width
let prefixGap = 1;          // spaces between > and text
// shift+knobs page 2
let prefixChar = 0;         // index into PREFIX_CHARS
const PREFIX_CHARS = [">", "\xBB", "-", "*", "="];
let unselIndent = 1;        // match prefix width (0=no indent, 1=match)

/* Page 3: Values & Edit */
let valueX = 92;
let valueAlignR = 0;        // 0=left, 1=right
let labelValGap = 6;
let valPadR = 2;
// shift+knobs page 3
let editBracket = 0;        // 0=[ ], 1=< >, 2=( ), 3={ }
const BRACKET_PAIRS = [["[","]"], ["<",">"], ["(",")"], ["{","}"]];
let editBracketPad = 1;     // spaces inside brackets

/* Page 4: Footer */
let footerShow = 1;
let footerTextY = 57;
let footerRuleY = 55;
let footerFontIdx = 0;      // 9px
// shift+knobs page 4
let footerLeftText = 0;
let footerRightText = 0;
const FOOTER_L_TEXTS = ["Back: exit", "Menu", "Cancel", "Done", ""];
const FOOTER_R_TEXTS = ["Jog: browse", "Shift: edit", "Enter: select", ""];

/* Page 5: Scroll & Sublabels */
let scrollIndX = 120;
let scrollTopY = 15;
let scrollBotY = 62;
let scrollArrowW = 5;
// shift+knobs page 5
let subLabelYOff = 9;
let subLabelXIndent = 12;
let subLabelLineBoost = 0;  // extra line height for sublabel items

/* ==================== STATE ==================== */
let currentPage = 0;
let shiftHeld = false;
const NUM_PAGES = 6;
const PAGE_NAMES = ["Title", "List", "Highlight", "Values", "Footer", "Scroll"];
const STEP_NOTES = [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31];

/* Status overlay */
let statusText = "";
let statusTimeout = 0;
const STATUS_DURATION = 44;

function showStatus(name, value) {
    statusText = `${name}: ${value}`;
    statusTimeout = STATUS_DURATION;
}

function fontPath(idx) {
    return FONT_BASE + FONT_SIZES[idx] + ".png";
}

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ==================== LED MANAGEMENT ==================== */

const LED_ACTIVE = 120;     // White
const LED_DIM = 124;        // DarkGrey
const LED_OFF = 0;

function updateStepLEDs() {
    for (let i = 0; i < 16; i++) {
        if (i < NUM_PAGES) {
            setLED(STEP_NOTES[i], i === currentPage ? LED_ACTIVE : LED_DIM);
        } else {
            setLED(STEP_NOTES[i], LED_OFF);
        }
    }
}

/* ==================== ARROW DRAWING ==================== */

function drawArrowUp(x, y, w) {
    const cx = x + Math.floor(w / 2);
    fill_rect(cx, y, 1, 1, 1);
    fill_rect(cx - 1, y + 1, 3, 1, 1);
    fill_rect(x, y + 2, w, 1, 1);
}

function drawArrowDown(x, y, w) {
    fill_rect(x, y, w, 1, 1);
    fill_rect(x + 1, y + 1, w - 2, 1, 1);
    fill_rect(x + Math.floor(w / 2), y + 2, 1, 1, 1);
}

/* ==================== DRAWING ==================== */

function drawScreen() {
    clear_screen();

    // --- Title ---
    set_font(fontPath(titleFontIdx));
    print(titleX, titleY, TITLE_TEXTS[titleText], 1);

    // Title rule
    fill_rect(0, titleRuleY, SCREEN_W, titleRuleThick, 1);

    // --- List items ---
    set_font(fontPath(listFontIdx));
    const fontH = get_font_height();
    const totalItems = DEMO_ITEMS.length;

    // Calculate effective line heights per item
    let visCount = listMaxVis;
    let startIdx = listScrollStart;

    // Auto-scroll to keep selected visible
    if (selectedItem < startIdx) startIdx = selectedItem;
    if (selectedItem >= startIdx + visCount) startIdx = selectedItem - visCount + 1;
    startIdx = clamp(startIdx, 0, Math.max(0, totalItems - visCount));
    listScrollStart = startIdx;

    const endIdx = Math.min(startIdx + visCount, totalItems);

    // Build prefix strings
    const pChar = PREFIX_CHARS[prefixChar];
    const gapStr = " ".repeat(prefixGap);
    const selPrefix = pChar + gapStr;
    const unselPrefix = unselIndent ? " ".repeat(selPrefix.length) : "";

    let y = listTopY;
    for (let i = startIdx; i < endIdx; i++) {
        const item = DEMO_ITEMS[i];
        const isSel = i === selectedItem;
        const prefix = isSel ? selPrefix : unselPrefix;
        const hasSub = !!item.subLabel;
        const itemH = hasSub ? listLineH + subLabelYOff + subLabelLineBoost : listLineH;

        // Highlight
        if (isSel) {
            const hy = y - hlYOff;
            const hh = hlHeight + hlPad * 2;
            fill_rect(0, hy, hlWidth, hh, 1);
        }

        // Label
        const labelStr = prefix + item.label;
        print(listLabelX, y, labelStr, isSel ? 0 : 1);

        // Value (if present)
        if (item.value) {
            let valStr = item.value;
            if (item.type === "edit" && isSel) {
                const bp = BRACKET_PAIRS[editBracket];
                const pad = " ".repeat(editBracketPad);
                valStr = bp[0] + pad + valStr + pad + bp[1];
            }
            // Enforce minimum gap between label and value
            const labelEnd = listLabelX + text_width(labelStr);
            const minValX = labelEnd + labelValGap;
            if (valueAlignR) {
                const tw = text_width(valStr);
                const rx = SCREEN_W - tw - valPadR;
                print(Math.max(rx, minValX), y, valStr, isSel ? 0 : 1);
            } else {
                print(Math.max(valueX, minValX), y, valStr, isSel ? 0 : 1);
            }
        }

        // Sublabel
        if (hasSub) {
            const subY = y + subLabelYOff;
            {
                print(listLabelX + subLabelXIndent, subY, item.subLabel, isSel ? 0 : 1);
            }
        }

        y += itemH;
    }

    // --- Footer (drawn over list to maximize pixels) ---
    if (footerShow) {
        set_font(fontPath(footerFontIdx));
        // Clear footer area then draw
        fill_rect(0, footerRuleY, SCREEN_W, SCREEN_H - footerRuleY, 0);
        fill_rect(0, footerRuleY, SCREEN_W, 1, 1);
        const fl = FOOTER_L_TEXTS[footerLeftText];
        const fr = FOOTER_R_TEXTS[footerRightText];
        if (fl) print(2, footerTextY, fl, 1);
        if (fr) {
            const tw = text_width(fr);
            print(SCREEN_W - tw - 2, footerTextY, fr, 1);
        }
    }

    // --- Scroll indicators (after footer so they aren't erased) ---
    if (startIdx > 0) {
        drawArrowUp(scrollIndX, scrollTopY, scrollArrowW);
    }
    if (endIdx < totalItems) {
        const botY = footerShow ? footerRuleY - 4 : scrollBotY - 2;
        drawArrowDown(scrollIndX, botY, scrollArrowW);
    }

    // --- Page indicator (top right, tiny) ---
    set_font(DEFAULT_FONT);
    const pageStr = PAGE_NAMES[currentPage] + (shiftHeld ? "+" : "");
    const pw = text_width(pageStr);
    // Draw over title area at right side
    fill_rect(SCREEN_W - pw - 2, 0, pw + 2, 8, 0);
    print(SCREEN_W - pw - 1, 0, pageStr, 1);

    // --- Status overlay ---
    if (statusTimeout > 0 && statusText) {
        set_font(DEFAULT_FONT);
        const textW = text_width(statusText);
        const boxW = textW + 8;
        const boxX = Math.floor((SCREEN_W - boxW) / 2);
        const boxY = 52;
        fill_rect(boxX, boxY, boxW, 12, 0);
        draw_rect(boxX, boxY, boxW, 12, 1);
        print(boxX + 4, boxY + 2, statusText, 1);
    }
}

/* ==================== CAPTURE ==================== */

function captureConfig() {
    host_ensure_dir(CAPTURE_DIR);
    captureCount++;
    const config = {
        capture: captureCount,
        title: {
            font: "tamzen-" + FONT_SIZES[titleFontIdx] + "px",
            x: titleX, y: titleY,
            rule_y: titleRuleY, rule_thick: titleRuleThick,
            text: TITLE_TEXTS[titleText]
        },
        list: {
            font: "tamzen-" + FONT_SIZES[listFontIdx] + "px",
            top_y: listTopY, line_height: listLineH,
            max_visible: listMaxVis, label_x: listLabelX
        },
        highlight: {
            height: hlHeight, y_offset: hlYOff,
            padding: hlPad, width: hlWidth
        },
        prefix: {
            char: PREFIX_CHARS[prefixChar],
            gap: prefixGap,
            unsel_indent: unselIndent
        },
        values: {
            x: valueX, align_right: valueAlignR,
            label_gap: labelValGap, pad_right: valPadR,
            bracket: BRACKET_PAIRS[editBracket],
            bracket_pad: editBracketPad
        },
        footer: {
            show: footerShow,
            font: "tamzen-" + FONT_SIZES[footerFontIdx] + "px",
            text_y: footerTextY, rule_y: footerRuleY,
            left: FOOTER_L_TEXTS[footerLeftText],
            right: FOOTER_R_TEXTS[footerRightText]
        },
        scroll: {
            indicator_x: scrollIndX, top_y: scrollTopY,
            bottom_y: scrollBotY, arrow_w: scrollArrowW
        },
        sublabel: {
            y_offset: subLabelYOff, x_indent: subLabelXIndent,
            line_boost: subLabelLineBoost
        }
    };
    const json = JSON.stringify(config, null, 2);
    const path = CAPTURE_DIR + "/capture_" + captureCount + ".json";
    host_write_file(path, json);
    showStatus("Captured", "#" + captureCount);
}

/* ==================== SAVE / LOAD LAYOUT ==================== */

const LAYOUT_PATH = "/data/UserData/schwung/ui-test-layout.json";

function saveLayout() {
    const layout = {
        titleFontIdx, titleX, titleY, titleRuleY, titleRuleThick, titleText,
        listFontIdx, listTopY, listLineH, listMaxVis, listLabelX,
        hlHeight, hlYOff, hlPad, hlWidth,
        prefixGap, prefixChar, unselIndent,
        valueX, valueAlignR, labelValGap, valPadR,
        editBracket, editBracketPad,
        footerShow, footerTextY, footerRuleY, footerFontIdx,
        footerLeftText, footerRightText,
        scrollIndX, scrollTopY, scrollBotY, scrollArrowW,
        subLabelYOff, subLabelXIndent, subLabelLineBoost
    };
    host_write_file(LAYOUT_PATH, JSON.stringify(layout, null, 2));
    showStatus("Layout", "saved");
}

function loadLayout() {
    const raw = host_read_file(LAYOUT_PATH);
    if (!raw) return;
    try {
        const l = JSON.parse(raw);
        if (l.titleFontIdx !== undefined) titleFontIdx = l.titleFontIdx;
        if (l.titleX !== undefined) titleX = l.titleX;
        if (l.titleY !== undefined) titleY = l.titleY;
        if (l.titleRuleY !== undefined) titleRuleY = l.titleRuleY;
        if (l.titleRuleThick !== undefined) titleRuleThick = l.titleRuleThick;
        if (l.titleText !== undefined) titleText = l.titleText;
        if (l.listFontIdx !== undefined) listFontIdx = l.listFontIdx;
        if (l.listTopY !== undefined) listTopY = l.listTopY;
        if (l.listLineH !== undefined) listLineH = l.listLineH;
        if (l.listMaxVis !== undefined) listMaxVis = l.listMaxVis;
        if (l.listLabelX !== undefined) listLabelX = l.listLabelX;
        if (l.hlHeight !== undefined) hlHeight = l.hlHeight;
        if (l.hlYOff !== undefined) hlYOff = l.hlYOff;
        if (l.hlPad !== undefined) hlPad = l.hlPad;
        if (l.hlWidth !== undefined) hlWidth = l.hlWidth;
        if (l.prefixGap !== undefined) prefixGap = l.prefixGap;
        if (l.prefixChar !== undefined) prefixChar = l.prefixChar;
        if (l.unselIndent !== undefined) unselIndent = l.unselIndent;
        if (l.valueX !== undefined) valueX = l.valueX;
        if (l.valueAlignR !== undefined) valueAlignR = l.valueAlignR;
        if (l.labelValGap !== undefined) labelValGap = l.labelValGap;
        if (l.valPadR !== undefined) valPadR = l.valPadR;
        if (l.editBracket !== undefined) editBracket = l.editBracket;
        if (l.editBracketPad !== undefined) editBracketPad = l.editBracketPad;
        if (l.footerShow !== undefined) footerShow = l.footerShow;
        if (l.footerTextY !== undefined) footerTextY = l.footerTextY;
        if (l.footerRuleY !== undefined) footerRuleY = l.footerRuleY;
        if (l.footerFontIdx !== undefined) footerFontIdx = l.footerFontIdx;
        if (l.footerLeftText !== undefined) footerLeftText = l.footerLeftText;
        if (l.footerRightText !== undefined) footerRightText = l.footerRightText;
        if (l.scrollIndX !== undefined) scrollIndX = l.scrollIndX;
        if (l.scrollTopY !== undefined) scrollTopY = l.scrollTopY;
        if (l.scrollBotY !== undefined) scrollBotY = l.scrollBotY;
        if (l.scrollArrowW !== undefined) scrollArrowW = l.scrollArrowW;
        if (l.subLabelYOff !== undefined) subLabelYOff = l.subLabelYOff;
        if (l.subLabelXIndent !== undefined) subLabelXIndent = l.subLabelXIndent;
        if (l.subLabelLineBoost !== undefined) subLabelLineBoost = l.subLabelLineBoost;
        showStatus("Layout", "loaded");
    } catch (e) { }
}

/* ==================== KNOB PARAMETER TABLES ==================== */

/*
 * Each page has up to 16 params: knobs 0-7 (normal) + 0-7 (shift).
 * Each entry: [name, getter, setter, min, max, isIndex]
 * isIndex = true means use +1/-1 stepping (for font/enum indices)
 */

function makeParam(name, get, set, min, max, isIdx) {
    return { name, get, set, min, max, isIdx: !!isIdx };
}

function getPageParams(page, shifted) {
    switch (page) {
        case 0: // Title
            if (!shifted) return [
                makeParam("T Font", () => FONT_SIZES[titleFontIdx] + "px", d => { titleFontIdx = clamp(titleFontIdx + d, 0, FONT_SIZES.length - 1); }, 0, FONT_SIZES.length - 1, true),
                makeParam("T X", () => titleX, d => { titleX = clamp(titleX + d, -10, 60); }, -10, 60),
                makeParam("T Y", () => titleY, d => { titleY = clamp(titleY + d, -10, 20); }, -10, 20),
                makeParam("Rule Y", () => titleRuleY, d => { titleRuleY = clamp(titleRuleY + d, -5, 30); }, -5, 30),
                null, null, null, null
            ];
            return [
                makeParam("Rule Th", () => titleRuleThick, d => { titleRuleThick = clamp(titleRuleThick + d, 0, 5); }, 0, 5),
                makeParam("T Text", () => TITLE_TEXTS[titleText], d => { titleText = clamp(titleText + d, 0, TITLE_TEXTS.length - 1); }, 0, TITLE_TEXTS.length - 1, true),
                null, null, null, null, null, null
            ];
        case 1: // List
            if (!shifted) return [
                makeParam("L Font", () => FONT_SIZES[listFontIdx] + "px", d => { listFontIdx = clamp(listFontIdx + d, 0, FONT_SIZES.length - 1); }, 0, FONT_SIZES.length - 1, true),
                makeParam("L Top", () => listTopY, d => { listTopY = clamp(listTopY + d, -5, 50); }, -5, 50),
                makeParam("Line H", () => listLineH, d => { listLineH = clamp(listLineH + d, 4, 25); }, 4, 25),
                makeParam("Max Vis", () => listMaxVis, d => { listMaxVis = clamp(listMaxVis + d, 1, 10); }, 1, 10, true),
                makeParam("Label X", () => listLabelX, d => { listLabelX = clamp(listLabelX + d, 0, 30); }, 0, 30),
                null, null, null
            ];
            return [
                makeParam("Sel Idx", () => selectedItem, d => { selectedItem = clamp(selectedItem + d, 0, DEMO_ITEMS.length - 1); }, 0, DEMO_ITEMS.length - 1, true),
                null, null, null, null, null, null, null
            ];
        case 2: // Highlight & Prefix
            if (!shifted) return [
                makeParam("HL H", () => hlHeight, d => { hlHeight = clamp(hlHeight + d, 1, 20); }, 1, 20),
                makeParam("HL Y", () => hlYOff, d => { hlYOff = clamp(hlYOff + d, -10, 10); }, -10, 10),
                makeParam("HL Pad", () => hlPad, d => { hlPad = clamp(hlPad + d, -5, 10); }, -5, 10),
                makeParam("HL W", () => hlWidth, d => { hlWidth = clamp(hlWidth + d, 10, 128); }, 10, 128),
                makeParam("Pfx Gap", () => prefixGap, d => { prefixGap = clamp(prefixGap + d, 0, 5); }, 0, 5),
                null, null, null
            ];
            return [
                makeParam("Pfx Ch", () => PREFIX_CHARS[prefixChar], d => { prefixChar = clamp(prefixChar + d, 0, PREFIX_CHARS.length - 1); }, 0, PREFIX_CHARS.length - 1, true),
                makeParam("Unsel", () => unselIndent ? "indent" : "flush", d => { unselIndent = clamp(unselIndent + d, 0, 1); }, 0, 1, true),
                null, null, null, null, null, null
            ];
        case 3: // Values & Edit
            if (!shifted) return [
                makeParam("Val X", () => valueX, d => { valueX = clamp(valueX + d, 30, 120); }, 30, 120),
                makeParam("Align R", () => valueAlignR ? "right" : "left", d => { valueAlignR = clamp(valueAlignR + d, 0, 1); }, 0, 1, true),
                makeParam("LV Gap", () => labelValGap, d => { labelValGap = clamp(labelValGap + d, 0, 20); }, 0, 20),
                makeParam("Val PadR", () => valPadR, d => { valPadR = clamp(valPadR + d, 0, 10); }, 0, 10),
                null, null, null, null
            ];
            return [
                makeParam("Bracket", () => BRACKET_PAIRS[editBracket].join(" "), d => { editBracket = clamp(editBracket + d, 0, BRACKET_PAIRS.length - 1); }, 0, BRACKET_PAIRS.length - 1, true),
                makeParam("Brk Pad", () => editBracketPad, d => { editBracketPad = clamp(editBracketPad + d, 0, 4); }, 0, 4),
                null, null, null, null, null, null
            ];
        case 4: // Footer
            if (!shifted) return [
                makeParam("Show", () => footerShow ? "on" : "off", d => { footerShow = clamp(footerShow + d, 0, 1); }, 0, 1, true),
                makeParam("Ftr Y", () => footerTextY, d => { footerTextY = clamp(footerTextY + d, 40, 63); }, 40, 63),
                makeParam("Ftr Rule", () => footerRuleY, d => { footerRuleY = clamp(footerRuleY + d, 38, 62); }, 38, 62),
                makeParam("F Font", () => FONT_SIZES[footerFontIdx] + "px", d => { footerFontIdx = clamp(footerFontIdx + d, 0, FONT_SIZES.length - 1); }, 0, FONT_SIZES.length - 1, true),
                null, null, null, null
            ];
            return [
                makeParam("Ftr L", () => FOOTER_L_TEXTS[footerLeftText], d => { footerLeftText = clamp(footerLeftText + d, 0, FOOTER_L_TEXTS.length - 1); }, 0, FOOTER_L_TEXTS.length - 1, true),
                makeParam("Ftr R", () => FOOTER_R_TEXTS[footerRightText], d => { footerRightText = clamp(footerRightText + d, 0, FOOTER_R_TEXTS.length - 1); }, 0, FOOTER_R_TEXTS.length - 1, true),
                null, null, null, null, null, null
            ];
        case 5: // Scroll & Sublabels
            if (!shifted) return [
                makeParam("Arr X", () => scrollIndX, d => { scrollIndX = clamp(scrollIndX + d, 80, 127); }, 80, 127),
                makeParam("Arr Top", () => scrollTopY, d => { scrollTopY = clamp(scrollTopY + d, 0, 40); }, 0, 40),
                makeParam("Arr Bot", () => scrollBotY, d => { scrollBotY = clamp(scrollBotY + d, 30, 64); }, 30, 64),
                makeParam("Arr W", () => scrollArrowW, d => { scrollArrowW = clamp(scrollArrowW + d, 3, 11); }, 3, 11),
                null, null, null, null
            ];
            return [
                makeParam("Sub Y", () => subLabelYOff, d => { subLabelYOff = clamp(subLabelYOff + d, 4, 16); }, 4, 16),
                makeParam("Sub X", () => subLabelXIndent, d => { subLabelXIndent = clamp(subLabelXIndent + d, 0, 30); }, 0, 30),
                makeParam("Sub +H", () => subLabelLineBoost, d => { subLabelLineBoost = clamp(subLabelLineBoost + d, 0, 10); }, 0, 10),
                null, null, null, null, null
            ];
    }
    return [null,null,null,null,null,null,null,null];
}

function getParamForKnob(knob) {
    const params = getPageParams(currentPage, shiftHeld);
    return params && knob < params.length ? params[knob] : null;
}

/* ==================== MIDI HANDLING ==================== */

globalThis.init = function() {
    loadLayout();
    updateStepLEDs();
    drawScreen();
};

globalThis.tick = function() {
    if (statusTimeout > 0) statusTimeout--;
    drawScreen();
};

globalThis.onMidiMessageInternal = function(data) {
    if (!data || data.length < 3) return;

    const status = data[0] & 0xF0;
    const note = data[1];
    const val = data[2];

    // Knob touch (Note On, notes 0-7) - show current value
    if (status === 0x90 && note <= 7 && val > 0) {
        const p = getParamForKnob(note);
        if (p) showStatus(p.name, p.get());
        return;
    }

    // Shift tracking (CC 49)
    if ((status === 0xB0) && note === 49) {
        shiftHeld = val > 0;
        return;
    }

    if (shouldFilterMessage(data)) return;
    const cc = note;

    // Back button
    if (status === 0xB0 && cc === 51 && val > 0) {
        // Clear step LEDs
        for (let i = 0; i < 16; i++) setLED(STEP_NOTES[i], LED_OFF);
        set_font(DEFAULT_FONT);
        host_exit_module();
        return;
    }

    // Menu button - capture config, shift+menu = save layout
    if (status === 0xB0 && cc === 50 && val > 0) {
        if (shiftHeld) {
            saveLayout();
        } else {
            captureConfig();
        }
        return;
    }

    // Step buttons (notes 16-31) - page select
    if (status === 0x90 && note >= 16 && note <= 31 && val > 0) {
        const page = note - 16;
        if (page < NUM_PAGES) {
            currentPage = page;
            updateStepLEDs();
            showStatus("Page", PAGE_NAMES[currentPage]);
        }
        return;
    }

    // Jog wheel turn - scroll selected item
    if (status === 0xB0 && cc === 14) {
        const delta = decodeDelta(val);
        if (delta !== 0) {
            selectedItem = clamp(selectedItem + (delta > 0 ? 1 : -1), 0, DEMO_ITEMS.length - 1);
            showStatus("Sel", selectedItem);
        }
        return;
    }

    // Knobs (CC 71-78)
    if (status === 0xB0 && cc >= 71 && cc <= 78) {
        const knob = cc - 71;
        const delta = decodeDelta(val);
        if (delta === 0) return;

        const p = getParamForKnob(knob);
        if (!p) return;

        const step = p.isIdx ? (delta > 0 ? 1 : -1) : delta;
        p.set(step);
        showStatus(p.name, p.get());
    }
};
