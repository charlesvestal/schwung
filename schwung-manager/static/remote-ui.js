/* ================================================================
   Remote UI - WebSocket client for shadow slot parameter control
   ================================================================ */

(function () {
    "use strict";

    var COMPONENT_KEYS = ["synth", "fx1", "fx2", "midi_fx1"];
    var COMPONENT_LABELS = {
        synth: "Synth",
        fx1: "Audio FX 1",
        fx2: "Audio FX 2",
        midi_fx1: "MIDI FX"
    };

    var MASTER_FX_KEYS = ["master_fx:fx1", "master_fx:fx2", "master_fx:fx3", "master_fx:fx4"];
    var MASTER_FX_LABELS = {
        "master_fx:fx1": "Master FX 1",
        "master_fx:fx2": "Master FX 2",
        "master_fx:fx3": "Master FX 3",
        "master_fx:fx4": "Master FX 4"
    };

    function makeComponent() {
        return { hierarchy: null, chainParams: null, params: {}, navStack: ["root"], module: "" };
    }

    function makeSlot() {
        return {
            components: {
                synth: makeComponent(),
                fx1: makeComponent(),
                fx2: makeComponent(),
                midi_fx1: makeComponent()
            },
            // Track which component sections are collapsed (true = collapsed).
            collapsed: { synth: false, fx1: true, fx2: true, midi_fx1: true },
            // Custom web UI URL for synth component (null = use auto-generated UI).
            customUI: null
        };
    }

    // Per-slot cached state.
    var slots = [makeSlot(), makeSlot(), makeSlot(), makeSlot()];

    // Master FX state.
    var masterFx = {
        components: {
            "master_fx:fx1": makeComponent(),
            "master_fx:fx2": makeComponent(),
            "master_fx:fx3": makeComponent(),
            "master_fx:fx4": makeComponent()
        },
        collapsed: { "master_fx:fx1": false, "master_fx:fx2": true, "master_fx:fx3": true, "master_fx:fx4": true }
    };

    var activeSlot = 0; // 0-3 for slots, "master" for master FX
    var ws = null;
    var reconnectTimer = null;
    var reconnectDelay = 1000; // Start at 1s, exponential backoff
    var reconnectAttempts = 0;
    var RECONNECT_DELAY_MIN = 1000;
    var RECONNECT_DELAY_MAX = 10000;
    var isConnected = false;
    var waitingForData = false; // True after subscribe, before first data arrives

    // Custom module web UI state.
    var customUIIframe = null; // Reference to active custom UI iframe element
    var customUISubscribed = false; // Whether the iframe has subscribed to param updates

    // Knob drag state.
    var dragging = null; // { component, key, startY, startValue, min, max, step, type, slot }
    var sendThrottleTimer = null;
    var SEND_INTERVAL = 16; // ~60Hz

    // DOM references.
    var statusEl = document.getElementById("remote-ui-status");
    var slotTitleEl = document.getElementById("slot-title");
    var slotContentEl = document.getElementById("slot-content");
    var debugEl = document.getElementById("slot-debug");
    var tabButtons = document.querySelectorAll(".remote-ui-tab");

    // ------------------------------------------------------------------
    // WebSocket connection
    // ------------------------------------------------------------------

    function connect() {
        if (ws && (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)) {
            return;
        }
        var proto = location.protocol === "https:" ? "wss:" : "ws:";
        ws = new WebSocket(proto + "//" + location.host + "/ws/remote-ui");

        ws.onopen = function () {
            reconnectDelay = RECONNECT_DELAY_MIN;
            reconnectAttempts = 0;
            isConnected = true;
            setStatus(true);
            waitingForData = true;
            renderSlot(); // Show loading state
            subscribe(activeSlot);
        };

        ws.onmessage = function (evt) {
            var msg;
            try {
                msg = JSON.parse(evt.data);
            } catch (e) {
                return;
            }
            if (waitingForData) {
                waitingForData = false;
            }
            dispatch(msg);
        };

        ws.onclose = function () {
            isConnected = false;
            setStatus(false);
            scheduleReconnect();
        };

        ws.onerror = function () {};
    }

    function scheduleReconnect() {
        if (reconnectTimer) return;
        reconnectAttempts++;
        reconnectTimer = setTimeout(function () {
            reconnectTimer = null;
            connect();
        }, reconnectDelay);
        // Update status with attempt count
        setStatus(false);
        // Exponential backoff: double delay, cap at max
        reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_DELAY_MAX);
    }

    function send(obj) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify(obj));
        }
    }

    function subscribe(slot) {
        if (slot === "master") {
            send({ type: "subscribe_master_fx" });
        } else {
            send({ type: "subscribe", slot: slot });
        }
    }

    function unsubscribe(slot) {
        if (slot === "master") {
            send({ type: "unsubscribe_master_fx" });
        } else {
            send({ type: "unsubscribe", slot: slot });
        }
    }

    // ------------------------------------------------------------------
    // Message dispatch
    // ------------------------------------------------------------------

    function dispatch(msg) {
        switch (msg.type) {
            case "master_fx_info":
                handleMasterFxInfo(msg);
                return;
            default:
                break;
        }

        var slot = msg.slot;

        // Check if this is a master FX message (slot 0 with master_fx: component prefix).
        var comp = msg.component || "";
        if (comp.indexOf("master_fx:") === 0) {
            switch (msg.type) {
                case "hierarchy":
                    handleMasterFxHierarchy(comp, msg);
                    break;
                case "chain_params":
                    handleMasterFxChainParams(comp, msg);
                    break;
            }
            return;
        }

        // Check if this is a param_update with master_fx keys.
        if (msg.type === "param_update" && msg.params) {
            var hasMasterFx = false;
            var hasSlot = false;
            for (var k in msg.params) {
                if (k.indexOf("master_fx:") === 0) hasMasterFx = true;
                else hasSlot = true;
            }
            if (hasMasterFx) {
                handleMasterFxParamUpdate(msg);
            }
            if (!hasSlot) return;
        }

        if (slot < 0 || slot > 3) return;

        switch (msg.type) {
            case "slot_info":
                handleSlotInfo(slot, msg);
                break;

            case "custom_ui":
                handleCustomUI(slot, msg);
                break;

            case "hierarchy":
                handleHierarchy(slot, msg);
                break;

            case "chain_params":
                handleChainParamsMsg(slot, msg);
                break;

            case "param_update":
                handleParamUpdate(slot, msg);
                break;

            default:
                break;
        }
    }

    function handleSlotInfo(slot, msg) {
        var s = slots[slot];
        // Clear custom UI if synth module changed.
        var prevSynth = s.components.synth.module;
        s.components.synth.module = msg.synth || "";
        s.components.fx1.module = msg.fx1 || "";
        s.components.fx2.module = msg.fx2 || "";
        s.components.midi_fx1.module = msg.midi_fx1 || "";
        if (prevSynth !== s.components.synth.module) {
            s.customUI = null;
        }
        if (slot === activeSlot) renderSlot();
    }

    function handleCustomUI(slot, msg) {
        if (slot < 0 || slot > 3) return;
        var s = slots[slot];
        s.customUI = { component: msg.component || "synth", url: msg.url || "" };
        if (slot === activeSlot) renderSlot();
    }

    function handleHierarchy(slot, msg) {
        var comp = msg.component || "synth";
        var c = slots[slot].components[comp];
        if (!c) return;
        c.hierarchy = msg.data;
        c.navStack = ["root"];
        if (slot === activeSlot) renderSlot();
    }

    function handleChainParamsMsg(slot, msg) {
        var comp = msg.component || "synth";
        var c = slots[slot].components[comp];
        if (!c) return;
        c.chainParams = msg.data;
        if (slot === activeSlot) renderSlot();
    }

    function handleParamUpdate(slot, msg) {
        if (!msg.params) return;
        var s = slots[slot];
        for (var prefixedKey in msg.params) {
            // Route to correct component based on prefix.
            var parts = splitPrefix(prefixedKey);
            var comp = s.components[parts.comp];
            if (comp) {
                comp.params[prefixedKey] = msg.params[prefixedKey];
            }
        }
        if (slot === activeSlot) updateParamValues(slot, msg.params);
        // Forward to custom UI iframe if subscribed.
        if (customUISubscribed && customUIIframe && slot === activeSlot) {
            postToIframe({ type: "paramUpdate", params: msg.params });
        }
    }

    // ------------------------------------------------------------------
    // Master FX message handlers
    // ------------------------------------------------------------------

    function handleMasterFxInfo(msg) {
        masterFx.components["master_fx:fx1"].module = msg.fx1 || "";
        masterFx.components["master_fx:fx2"].module = msg.fx2 || "";
        masterFx.components["master_fx:fx3"].module = msg.fx3 || "";
        masterFx.components["master_fx:fx4"].module = msg.fx4 || "";
        if (activeSlot === "master") renderSlot();
    }

    function handleMasterFxHierarchy(comp, msg) {
        var c = masterFx.components[comp];
        if (!c) return;
        c.hierarchy = msg.data;
        c.navStack = ["root"];
        if (activeSlot === "master") renderSlot();
    }

    function handleMasterFxChainParams(comp, msg) {
        var c = masterFx.components[comp];
        if (!c) return;
        c.chainParams = msg.data;
        if (activeSlot === "master") renderSlot();
    }

    function handleMasterFxParamUpdate(msg) {
        if (!msg.params) return;
        for (var fullKey in msg.params) {
            // fullKey is like "master_fx:fx1:cutoff"
            var parts = splitMasterFxPrefix(fullKey);
            if (parts) {
                var comp = masterFx.components[parts.comp];
                if (comp) {
                    comp.params[fullKey] = msg.params[fullKey];
                }
            }
        }
        if (activeSlot === "master") updateMasterFxParamValues(msg.params);
    }

    /** Split "master_fx:fx1:cutoff" -> { comp: "master_fx:fx1", key: "cutoff" }. */
    function splitMasterFxPrefix(fullKey) {
        for (var i = 0; i < MASTER_FX_KEYS.length; i++) {
            var p = MASTER_FX_KEYS[i] + ":";
            if (fullKey.indexOf(p) === 0) {
                return { comp: MASTER_FX_KEYS[i], key: fullKey.substring(p.length) };
            }
        }
        return null;
    }

    /** Split "fx1:cutoff" -> { comp: "fx1", key: "cutoff" }. Default comp is "synth". */
    function splitPrefix(prefixedKey) {
        for (var i = 0; i < COMPONENT_KEYS.length; i++) {
            var p = COMPONENT_KEYS[i] + ":";
            if (prefixedKey.indexOf(p) === 0) {
                return { comp: COMPONENT_KEYS[i], key: prefixedKey.substring(p.length) };
            }
        }
        return { comp: "synth", key: prefixedKey };
    }

    // ------------------------------------------------------------------
    // Slot switching
    // ------------------------------------------------------------------

    function switchSlot(n) {
        if (n === activeSlot) return;
        unsubscribe(activeSlot);
        activeSlot = n;
        waitingForData = true;

        tabButtons.forEach(function (btn) {
            var slotAttr = btn.getAttribute("data-slot");
            var match = (slotAttr === "master") ? (n === "master") : (parseInt(slotAttr, 10) === n);
            if (match) {
                btn.classList.add("active");
                btn.setAttribute("aria-selected", "true");
            } else {
                btn.classList.remove("active");
                btn.setAttribute("aria-selected", "false");
            }
        });

        subscribe(n);
        renderSlot();
    }

    window.switchSlot = switchSlot;

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    function escapeHtml(str) {
        var div = document.createElement("div");
        div.appendChild(document.createTextNode(str));
        return div.innerHTML;
    }

    /** Look up chain_params metadata for a param key within a component. */
    function findParamMeta(compState, key) {
        var cp = compState.chainParams;
        if (!cp) return null;
        for (var i = 0; i < cp.length; i++) {
            if (cp[i].key === key) return cp[i];
        }
        return null;
    }

    /** Get param value from component state (with prefix). */
    function getCompParamValue(compState, compPrefix, key) {
        var v = compState.params[compPrefix + ":" + key];
        if (v !== undefined) return v;
        return compState.params[key];
    }

    /** Get the current hierarchy level object for a component. */
    function getCompCurrentLevel(compState) {
        if (!compState.hierarchy || !compState.hierarchy.levels) return null;
        var stack = compState.navStack;
        var levelName = stack[stack.length - 1];
        return compState.hierarchy.levels[levelName] || null;
    }

    /** Resolve children: if a level has "children", auto-navigate. */
    function resolveCompLevel(compState) {
        if (!compState.hierarchy || !compState.hierarchy.levels) return;
        var maxDepth = 10;
        while (maxDepth-- > 0) {
            var levelName = compState.navStack[compState.navStack.length - 1];
            var level = compState.hierarchy.levels[levelName];
            if (!level || !level.children) break;
            var child = level.children;
            if (compState.hierarchy.levels[child]) {
                compState.navStack.push(child);
            } else {
                break;
            }
        }
    }

    function clampValue(val, meta) {
        if (meta.min !== undefined && val < meta.min) val = meta.min;
        if (meta.max !== undefined && val > meta.max) val = meta.max;
        return val;
    }

    function formatValue(val, meta) {
        if (meta && meta.type === "enum" && meta.options) {
            var idx = parseInt(val, 10);
            if (idx >= 0 && idx < meta.options.length) return meta.options[idx];
            return String(val);
        }
        if (meta && meta.type === "float") {
            var f = parseFloat(val);
            if (!isNaN(f)) return f.toFixed(2);
        }
        return String(val);
    }

    // ------------------------------------------------------------------
    // SVG Knob
    // ------------------------------------------------------------------

    var KNOB_SIZE = 60;
    var KNOB_RADIUS = 24;
    var ARC_START_DEG = 225;
    var ARC_END_DEG = 135;
    var ARC_SWEEP_DEG = 270;

    function degToRad(d) { return d * Math.PI / 180; }

    function polarToXY(cx, cy, r, deg) {
        var rad = degToRad(deg - 90);
        return { x: cx + r * Math.cos(rad), y: cy + r * Math.sin(rad) };
    }

    function describeArc(cx, cy, r, startDeg, sweepDeg) {
        var endDeg = startDeg + sweepDeg;
        var start = polarToXY(cx, cy, r, startDeg);
        var end = polarToXY(cx, cy, r, endDeg);
        var largeArc = sweepDeg > 180 ? 1 : 0;
        return "M " + start.x + " " + start.y +
               " A " + r + " " + r + " 0 " + largeArc + " 1 " + end.x + " " + end.y;
    }

    function valueToNorm(val, meta) {
        var min = (meta && meta.min !== undefined) ? meta.min : 0;
        var max = (meta && meta.max !== undefined) ? meta.max : 1;
        if (max === min) return 0;
        var t = (val - min) / (max - min);
        return Math.max(0, Math.min(1, t));
    }

    function normToAngle(t) {
        return ARC_START_DEG + t * ARC_SWEEP_DEG;
    }

    function createKnobSVG(key, meta, value) {
        var cx = KNOB_SIZE / 2;
        var cy = KNOB_SIZE / 2;
        var numVal = parseFloat(value);
        if (isNaN(numVal)) numVal = meta ? (meta.min || 0) : 0;

        var t = valueToNorm(numVal, meta);
        var indicatorAngle = normToAngle(t);
        var indicator = polarToXY(cx, cy, KNOB_RADIUS - 4, indicatorAngle);

        var bgPath = describeArc(cx, cy, KNOB_RADIUS, ARC_START_DEG, ARC_SWEEP_DEG);

        var valSweep = t * ARC_SWEEP_DEG;
        var valPath = "";
        if (valSweep > 1) {
            valPath = describeArc(cx, cy, KNOB_RADIUS, ARC_START_DEG, valSweep);
        }

        var svg = '<svg class="knob-svg" width="' + KNOB_SIZE + '" height="' + KNOB_SIZE + '" viewBox="0 0 ' + KNOB_SIZE + ' ' + KNOB_SIZE + '">';
        svg += '<path d="' + bgPath + '" fill="none" stroke="#444" stroke-width="4" stroke-linecap="round"/>';
        if (valPath) {
            svg += '<path class="knob-value-arc" d="' + valPath + '" fill="none" stroke="#e8a84c" stroke-width="4" stroke-linecap="round"/>';
        }
        svg += '<circle cx="' + cx + '" cy="' + cy + '" r="3" fill="#888"/>';
        svg += '<line class="knob-indicator" x1="' + cx + '" y1="' + cy + '" x2="' + indicator.x + '" y2="' + indicator.y + '" stroke="#e8a84c" stroke-width="2" stroke-linecap="round"/>';
        svg += '</svg>';
        return svg;
    }

    function renderKnob(compPrefix, compState, key, meta, value) {
        var container = document.createElement("div");
        container.className = "knob-container";
        container.setAttribute("data-param-key", compPrefix + ":" + key);

        var label = (meta && meta.name) ? meta.name : key;
        var displayVal = formatValue(value !== undefined ? value : (meta ? (meta.min || 0) : 0), meta);

        container.innerHTML =
            createKnobSVG(key, meta, value !== undefined ? value : (meta ? (meta.min || 0) : 0)) +
            '<div class="knob-label">' + escapeHtml(label) + '</div>' +
            '<div class="knob-value" data-knob-value="' + escapeHtml(compPrefix + ":" + key) + '">' + escapeHtml(displayVal) + '</div>';

        // Drag handling
        function onPointerDown(e) {
            e.preventDefault();
            var numVal = parseFloat(getCompParamValue(compState, compPrefix, key));
            if (isNaN(numVal)) numVal = meta ? (meta.min || 0) : 0;
            var clientY = e.touches ? e.touches[0].clientY : e.clientY;
            dragging = {
                component: compPrefix,
                key: key,
                startY: clientY,
                startValue: numVal,
                min: meta ? (meta.min !== undefined ? meta.min : 0) : 0,
                max: meta ? (meta.max !== undefined ? meta.max : 1) : 1,
                step: meta ? (meta.step || (meta.type === "int" ? 1 : 0.01)) : 0.01,
                type: meta ? meta.type : "float",
                slot: activeSlot
            };
            document.addEventListener("mousemove", onPointerMove);
            document.addEventListener("mouseup", onPointerUp);
            document.addEventListener("touchmove", onPointerMove, { passive: false });
            document.addEventListener("touchend", onPointerUp);
        }

        container.addEventListener("mousedown", onPointerDown);
        container.addEventListener("touchstart", onPointerDown, { passive: false });

        return container;
    }

    function onPointerMove(e) {
        if (!dragging) return;
        e.preventDefault();
        var clientY = e.touches ? e.touches[0].clientY : e.clientY;
        var dy = dragging.startY - clientY;
        var range = dragging.max - dragging.min;
        var sensitivity = range / 150;
        var newVal = dragging.startValue + dy * sensitivity;

        if (dragging.step > 0) {
            newVal = Math.round(newVal / dragging.step) * dragging.step;
        }
        newVal = clampValue(newVal, dragging);
        if (dragging.type === "int") newVal = Math.round(newVal);

        var prefixedKey = dragging.component + ":" + dragging.key;
        var comp = getComponentForDrag(dragging.slot, dragging.component);
        if (comp) comp.params[prefixedKey] = String(newVal);

        updateKnobVisual(prefixedKey, newVal);

        if (!sendThrottleTimer) {
            sendThrottleTimer = setTimeout(function () {
                sendThrottleTimer = null;
            }, SEND_INTERVAL);
            sendParamValue(dragging.slot, dragging.component, dragging.key, newVal);
        }
    }

    function onPointerUp(e) {
        if (!dragging) return;
        var prefixedKey = dragging.component + ":" + dragging.key;
        var comp = getComponentForDrag(dragging.slot, dragging.component);
        var finalVal = comp ? comp.params[prefixedKey] : "0";
        sendParamValue(dragging.slot, dragging.component, dragging.key, parseFloat(finalVal));
        dragging = null;
        document.removeEventListener("mousemove", onPointerMove);
        document.removeEventListener("mouseup", onPointerUp);
        document.removeEventListener("touchmove", onPointerMove);
        document.removeEventListener("touchend", onPointerUp);
    }

    /** Helper to get component state for a given slot and component key. */
    function getComponentForDrag(slot, compKey) {
        if (slot === "master") {
            return masterFx.components[compKey] || null;
        }
        return slots[slot] ? slots[slot].components[compKey] : null;
    }

    /** Update SVG knob in-place by modifying attributes directly (no DOM rebuild). */
    function updateKnobSVGInPlace(svgEl, value, meta) {
        var cx = KNOB_SIZE / 2;
        var cy = KNOB_SIZE / 2;
        var numVal = parseFloat(value);
        if (isNaN(numVal)) numVal = meta ? (meta.min || 0) : 0;

        var t = valueToNorm(numVal, meta);
        var indicatorAngle = normToAngle(t);
        var indicator = polarToXY(cx, cy, KNOB_RADIUS - 4, indicatorAngle);

        // Update value arc
        var valArc = svgEl.querySelector(".knob-value-arc");
        var valSweep = t * ARC_SWEEP_DEG;
        if (valSweep > 1) {
            var valPath = describeArc(cx, cy, KNOB_RADIUS, ARC_START_DEG, valSweep);
            if (valArc) {
                valArc.setAttribute("d", valPath);
            } else {
                // Create value arc if it didn't exist (was at 0)
                valArc = document.createElementNS("http://www.w3.org/2000/svg", "path");
                valArc.setAttribute("class", "knob-value-arc");
                valArc.setAttribute("fill", "none");
                valArc.setAttribute("stroke", "#e8a84c");
                valArc.setAttribute("stroke-width", "4");
                valArc.setAttribute("stroke-linecap", "round");
                valArc.setAttribute("d", valPath);
                // Insert after bg arc (first child)
                var bgArc = svgEl.querySelector("path");
                if (bgArc && bgArc.nextSibling) {
                    svgEl.insertBefore(valArc, bgArc.nextSibling);
                } else {
                    svgEl.appendChild(valArc);
                }
            }
        } else if (valArc) {
            valArc.removeAttribute("d");
        }

        // Update indicator line
        var line = svgEl.querySelector(".knob-indicator");
        if (line) {
            line.setAttribute("x2", indicator.x);
            line.setAttribute("y2", indicator.y);
        }
    }

    // Active knob animations: prefixedKey -> { from, to, start, duration, raf }
    var knobAnimations = {};
    var KNOB_ANIM_DURATION = 80; // ms — smooth but fast

    /** Immediate knob update (used during user drag). */
    function updateKnobVisual(prefixedKey, value) {
        // Cancel any running animation for this knob
        if (knobAnimations[prefixedKey]) {
            cancelAnimationFrame(knobAnimations[prefixedKey].raf);
            delete knobAnimations[prefixedKey];
        }
        updateKnobVisualDirect(prefixedKey, value);
    }

    /** Animated knob update (used for incoming device changes). */
    function updateKnobVisualAnimated(prefixedKey, targetValue) {
        var container = slotContentEl.querySelector('[data-param-key="' + prefixedKey + '"]');
        if (!container) return;

        var meta = getKnobMeta(prefixedKey);
        if (!meta) { updateKnobVisualDirect(prefixedKey, targetValue); return; }

        // Get current displayed value
        var svgEl = container.querySelector(".knob-svg");
        if (!svgEl) { updateKnobVisualDirect(prefixedKey, targetValue); return; }

        var valEl = container.querySelector('[data-knob-value="' + prefixedKey + '"]');
        var currentText = valEl ? valEl.textContent : "0";
        var currentVal = parseFloat(currentText);
        if (isNaN(currentVal)) currentVal = meta.min || 0;
        var targetVal = parseFloat(targetValue);
        if (isNaN(targetVal)) targetVal = meta.min || 0;

        // Skip animation if change is tiny
        var range = (meta.max || 1) - (meta.min || 0);
        if (range > 0 && Math.abs(targetVal - currentVal) / range < 0.005) {
            updateKnobVisualDirect(prefixedKey, targetValue);
            return;
        }

        // Cancel existing animation
        if (knobAnimations[prefixedKey]) {
            cancelAnimationFrame(knobAnimations[prefixedKey].raf);
        }

        var anim = { from: currentVal, to: targetVal, start: performance.now(), duration: KNOB_ANIM_DURATION };
        knobAnimations[prefixedKey] = anim;

        function step(now) {
            var elapsed = now - anim.start;
            var t = Math.min(1, elapsed / anim.duration);
            // Ease out cubic
            var ease = 1 - Math.pow(1 - t, 3);
            var val = anim.from + (anim.to - anim.from) * ease;

            updateKnobSVGInPlace(svgEl, val, meta);
            if (valEl) valEl.textContent = formatValue(val, meta);

            if (t < 1) {
                anim.raf = requestAnimationFrame(step);
            } else {
                delete knobAnimations[prefixedKey];
            }
        }
        anim.raf = requestAnimationFrame(step);
    }

    function getKnobMeta(prefixedKey) {
        var compState;
        var parts;
        if (activeSlot === "master") {
            parts = splitMasterFxPrefix(prefixedKey);
            if (parts) compState = masterFx.components[parts.comp];
        }
        if (!compState) {
            parts = splitPrefix(prefixedKey);
            compState = slots[typeof activeSlot === "number" ? activeSlot : 0].components[parts.comp];
        }
        if (!compState || !parts) return null;
        return findParamMeta(compState, parts.key);
    }

    function updateKnobVisualDirect(prefixedKey, value) {
        var container = slotContentEl.querySelector('[data-param-key="' + prefixedKey + '"]');
        if (!container) return;

        var meta = getKnobMeta(prefixedKey);
        var svgEl = container.querySelector(".knob-svg");
        if (svgEl) {
            updateKnobSVGInPlace(svgEl, value, meta);
        }
        var valEl = container.querySelector('[data-knob-value="' + prefixedKey + '"]');
        if (valEl) {
            valEl.textContent = formatValue(value, meta);
        }
    }

    // ------------------------------------------------------------------
    // Send param
    // ------------------------------------------------------------------

    function sendParamValue(slot, compPrefix, key, value) {
        if (slot === "master") {
            send({
                type: "set_master_fx_param",
                key: compPrefix + ":" + key,
                value: String(value)
            });
        } else {
            send({
                type: "set_param",
                slot: slot,
                key: compPrefix + ":" + key,
                value: String(value)
            });
        }
    }

    // ------------------------------------------------------------------
    // Preset browser
    // ------------------------------------------------------------------

    function renderPresetBrowser(level, compPrefix, compState) {
        if (!level.list_param || !level.count_param) return null;

        var countVal = getCompParamValue(compState, compPrefix, level.count_param);
        var currentVal = getCompParamValue(compState, compPrefix, level.list_param);
        var nameVal = level.name_param ? getCompParamValue(compState, compPrefix, level.name_param) : null;

        var count = parseInt(countVal, 10) || 0;
        var current = parseInt(currentVal, 10) || 0;

        var container = document.createElement("div");
        container.className = "preset-browser";
        container.setAttribute("data-preset-browser", compPrefix);

        var prevBtn = document.createElement("button");
        prevBtn.className = "preset-nav-btn";
        prevBtn.textContent = "\u25C0";
        prevBtn.disabled = current <= 0;
        prevBtn.onclick = function () {
            if (current > 0) {
                sendParamValue(activeSlot, compPrefix, level.list_param, current - 1);
            }
        };

        var nextBtn = document.createElement("button");
        nextBtn.className = "preset-nav-btn";
        nextBtn.textContent = "\u25B6";
        nextBtn.disabled = current >= count - 1;
        nextBtn.onclick = function () {
            if (current < count - 1) {
                sendParamValue(activeSlot, compPrefix, level.list_param, current + 1);
            }
        };

        var nameSpan = document.createElement("span");
        nameSpan.className = "preset-name";
        nameSpan.setAttribute("data-preset-name", compPrefix);
        var displayName = nameVal || ("Preset " + current);
        nameSpan.textContent = (current + 1) + "/" + count + "  " + displayName;

        container.appendChild(prevBtn);
        container.appendChild(nameSpan);
        container.appendChild(nextBtn);
        return container;
    }

    // ------------------------------------------------------------------
    // Param list items
    // ------------------------------------------------------------------

    function renderParamItem(entry, compPrefix, compState) {
        var key = null;
        var label = null;
        var navLevel = null;

        if (typeof entry === "string") {
            key = entry;
        } else if (entry && entry.level) {
            navLevel = entry.level;
            label = entry.label || entry.level;
        } else if (entry && entry.key) {
            key = entry.key;
            label = entry.label;
        }

        // Navigation link
        if (navLevel) {
            var row = document.createElement("div");
            row.className = "param-row param-nav-link";
            row.setAttribute("data-nav-level", navLevel);
            row.innerHTML = '<span class="param-nav-label">' + escapeHtml(label) + '</span>' +
                            '<span class="param-nav-arrow">\u203A</span>';
            row.onclick = function () {
                compState.navStack.push(navLevel);
                resolveCompLevel(compState);
                renderSlot();
            };
            return row;
        }

        // Editable param
        if (!key) return null;
        var meta = findParamMeta(compState, key);
        var value = getCompParamValue(compState, compPrefix, key);
        if (!label && meta) label = meta.name;
        if (!label) label = key;

        var row = document.createElement("div");
        row.className = "param-row";
        row.setAttribute("data-param-row", compPrefix + ":" + key);

        var labelSpan = document.createElement("span");
        labelSpan.className = "param-label";
        labelSpan.textContent = label;
        row.appendChild(labelSpan);

        var controlWrap = document.createElement("div");
        controlWrap.className = "param-control";

        if (!meta) {
            var valSpan = document.createElement("span");
            valSpan.className = "param-value-text";
            valSpan.setAttribute("data-param-value", compPrefix + ":" + key);
            valSpan.textContent = value !== undefined ? String(value) : "?";
            controlWrap.appendChild(valSpan);
        } else if (meta.type === "enum" && meta.options) {
            var select = document.createElement("select");
            select.className = "param-select";
            select.setAttribute("data-param-input", compPrefix + ":" + key);
            for (var i = 0; i < meta.options.length; i++) {
                var opt = document.createElement("option");
                opt.value = String(i);
                opt.textContent = meta.options[i];
                if (String(i) === String(value)) opt.selected = true;
                select.appendChild(opt);
            }
            select.onchange = function () {
                var v = select.value;
                compState.params[compPrefix + ":" + key] = v;
                sendParamValue(activeSlot, compPrefix, key, parseInt(v, 10));
            };
            controlWrap.appendChild(select);
        } else if (meta.type === "float" || meta.type === "int") {
            var slider = document.createElement("input");
            slider.type = "range";
            slider.className = "param-slider";
            slider.setAttribute("data-param-input", compPrefix + ":" + key);
            slider.min = meta.min !== undefined ? meta.min : 0;
            slider.max = meta.max !== undefined ? meta.max : (meta.type === "int" ? 127 : 1);
            slider.step = meta.step || (meta.type === "int" ? 1 : 0.01);
            slider.value = value !== undefined ? value : slider.min;

            var valDisplay = document.createElement("span");
            valDisplay.className = "param-slider-value";
            valDisplay.setAttribute("data-param-value", compPrefix + ":" + key);
            valDisplay.textContent = formatValue(value !== undefined ? value : slider.min, meta);

            (function(capturedKey, capturedComp, capturedMeta) {
                slider.oninput = function () {
                    var v = capturedMeta.type === "int" ? parseInt(slider.value, 10) : parseFloat(slider.value);
                    compState.params[capturedComp + ":" + capturedKey] = String(v);
                    valDisplay.textContent = formatValue(v, capturedMeta);
                    if (!sendThrottleTimer) {
                        sendThrottleTimer = setTimeout(function () {
                            sendThrottleTimer = null;
                        }, SEND_INTERVAL);
                        sendParamValue(activeSlot, capturedComp, capturedKey, v);
                    }
                };
                slider.onchange = function () {
                    var v = capturedMeta.type === "int" ? parseInt(slider.value, 10) : parseFloat(slider.value);
                    sendParamValue(activeSlot, capturedComp, capturedKey, v);
                };
            })(key, compPrefix, meta);

            controlWrap.appendChild(slider);
            controlWrap.appendChild(valDisplay);
        } else {
            var valSpan2 = document.createElement("span");
            valSpan2.className = "param-value-text";
            valSpan2.setAttribute("data-param-value", compPrefix + ":" + key);
            valSpan2.textContent = value !== undefined ? String(value) : "?";
            controlWrap.appendChild(valSpan2);
        }

        row.appendChild(controlWrap);
        return row;
    }

    // ------------------------------------------------------------------
    // Breadcrumb (per-component)
    // ------------------------------------------------------------------

    function renderBreadcrumb(compState, compPrefix, containerEl) {
        containerEl.innerHTML = "";
        if (!compState.hierarchy || !compState.hierarchy.levels) return;

        var stack = compState.navStack;
        for (var i = 0; i < stack.length; i++) {
            if (i > 0) {
                var sep = document.createElement("span");
                sep.className = "breadcrumb-sep";
                sep.textContent = " > ";
                containerEl.appendChild(sep);
            }

            var levelName = stack[i];
            var levelObj = compState.hierarchy.levels[levelName];
            var label = (levelObj && levelObj.label) ? levelObj.label : levelName;

            if (i < stack.length - 1) {
                var link = document.createElement("a");
                link.textContent = label;
                link.setAttribute("data-nav-index", String(i));
                link.onclick = (function (idx, cs) {
                    return function () {
                        cs.navStack = cs.navStack.slice(0, idx + 1);
                        renderSlot();
                    };
                })(i, compState);
                containerEl.appendChild(link);
            } else {
                var span = document.createElement("span");
                span.className = "breadcrumb-current";
                span.textContent = label;
                containerEl.appendChild(span);
            }
        }
    }

    // ------------------------------------------------------------------
    // Incremental param value updates (avoid full re-render)
    // ------------------------------------------------------------------

    function updateParamValues(slot, changedParams) {
        if (slot !== activeSlot) return;
        if (!changedParams) { renderSlot(); return; }

        var needsFullRender = false;

        for (var prefixedKey in changedParams) {
            var value = changedParams[prefixedKey];
            var parts = splitPrefix(prefixedKey);
            var compState = slots[slot].components[parts.comp];
            if (!compState) continue;

            // Skip if user is dragging this knob
            if (dragging && dragging.component === parts.comp && dragging.key === parts.key && dragging.slot === slot) continue;

            // Try to update knob visual (animated for smooth transitions)
            var knobContainer = slotContentEl.querySelector('[data-param-key="' + prefixedKey + '"]');
            if (knobContainer) {
                updateKnobVisualAnimated(prefixedKey, parseFloat(value));
            }

            // Try to update param row control
            var paramRow = slotContentEl.querySelector('[data-param-row="' + prefixedKey + '"]');
            if (paramRow) {
                var input = paramRow.querySelector('[data-param-input="' + prefixedKey + '"]');
                if (input) {
                    if (input.tagName === "SELECT") {
                        input.value = String(value);
                    } else if (input.type === "range") {
                        input.value = value;
                    }
                }
                var valDisplay = paramRow.querySelector('[data-param-value="' + prefixedKey + '"]');
                if (valDisplay) {
                    var meta = findParamMeta(compState, parts.key);
                    valDisplay.textContent = formatValue(value, meta);
                }
            }

            // Update preset browser if relevant
            var presetBrowser = slotContentEl.querySelector('[data-preset-browser="' + parts.comp + '"]');
            if (presetBrowser) {
                var level = getCompCurrentLevel(compState);
                if (level && (parts.key === level.list_param ||
                              parts.key === level.name_param ||
                              parts.key === level.count_param)) {
                    needsFullRender = true;
                }
            }
        }

        if (needsFullRender) renderSlot();
    }

    // ------------------------------------------------------------------
    // Render a single component section
    // ------------------------------------------------------------------

    function renderComponentSection(compKey, compState, isCollapsed) {
        var section = document.createElement("div");
        section.className = "component-section";
        section.setAttribute("data-component", compKey);

        // Header bar
        var header = document.createElement("div");
        header.className = "component-header" + (isCollapsed ? " collapsed" : "");
        var arrow = document.createElement("span");
        arrow.className = "component-toggle";
        arrow.textContent = isCollapsed ? "\u25B6" : "\u25BC";
        var title = document.createElement("span");
        title.className = "component-title";
        var displayLabel = COMPONENT_LABELS[compKey] || compKey;
        var moduleName = compState.module || "";
        title.textContent = displayLabel + (moduleName ? " - " + moduleName : "");
        header.appendChild(arrow);
        header.appendChild(title);
        header.onclick = function () {
            slots[activeSlot].collapsed[compKey] = !slots[activeSlot].collapsed[compKey];
            renderSlot();
        };
        section.appendChild(header);

        if (isCollapsed) return section;

        // Body
        var body = document.createElement("div");
        body.className = "component-body";

        // Resolve auto-navigation
        resolveCompLevel(compState);

        // Breadcrumb
        if (compState.hierarchy && compState.hierarchy.levels && compState.navStack.length > 1) {
            var bcEl = document.createElement("nav");
            bcEl.className = "slot-breadcrumb";
            renderBreadcrumb(compState, compKey, bcEl);
            body.appendChild(bcEl);
        }

        var level = getCompCurrentLevel(compState);
        if (!level) {
            var noLevel = document.createElement("p");
            noLevel.className = "text-muted";
            noLevel.textContent = "No parameters available";
            body.appendChild(noLevel);
            section.appendChild(body);
            return section;
        }

        // Preset browser
        if (level.list_param && level.count_param) {
            var presetEl = renderPresetBrowser(level, compKey, compState);
            if (presetEl) body.appendChild(presetEl);
        }

        // Knob row
        var knobs = level.knobs || [];
        if (knobs.length > 0) {
            var knobRow = document.createElement("div");
            knobRow.className = "knob-row";
            for (var i = 0; i < knobs.length && i < 8; i++) {
                var knobKey = knobs[i];
                var meta = findParamMeta(compState, knobKey);
                var value = getCompParamValue(compState, compKey, knobKey);
                var knobEl = renderKnob(compKey, compState, knobKey, meta, value);
                knobRow.appendChild(knobEl);
            }
            body.appendChild(knobRow);
        }

        // Param list
        var params = level.params || [];
        if (params.length > 0) {
            var paramList = document.createElement("div");
            paramList.className = "param-list";
            for (var j = 0; j < params.length; j++) {
                var item = renderParamItem(params[j], compKey, compState);
                if (item) paramList.appendChild(item);
            }
            body.appendChild(paramList);
        }

        // Empty state
        if (knobs.length === 0 && params.length === 0 && !level.list_param) {
            var empty = document.createElement("p");
            empty.className = "text-muted";
            empty.textContent = "No parameters on this level";
            body.appendChild(empty);
        }

        section.appendChild(body);
        return section;
    }

    // ------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------

    function renderSlot() {
        slotContentEl.innerHTML = "";
        debugEl.innerHTML = "";

        // Clear custom UI iframe state on re-render.
        customUIIframe = null;
        customUISubscribed = false;

        if (activeSlot === "master") {
            renderMasterFx();
            return;
        }

        var s = slots[activeSlot];
        slotTitleEl.textContent = "Slot " + (activeSlot + 1);

        // Check if any component has data
        var hasAnyData = false;
        var hasAnyModule = false;
        for (var i = 0; i < COMPONENT_KEYS.length; i++) {
            var comp = s.components[COMPONENT_KEYS[i]];
            if (comp.module) hasAnyModule = true;
            if (comp.hierarchy || comp.chainParams || Object.keys(comp.params).length > 0) {
                hasAnyData = true;
            }
        }

        if (!hasAnyData && !hasAnyModule) {
            if (waitingForData || !isConnected) {
                debugEl.innerHTML = '<div class="loading-state"><span class="loading-spinner"></span> Loading slot...</div>';
            } else {
                debugEl.innerHTML = '<p class="text-muted">No module loaded</p>';
            }
            return;
        }

        if (!hasAnyModule) {
            slotContentEl.innerHTML = '<p class="text-muted">No module loaded</p>';
            return;
        }

        // Check for custom web UI on synth component.
        if (s.customUI && s.customUI.url) {
            renderCustomUI(s);
            return;
        }

        // Render component sections for loaded components
        var renderedCount = 0;
        for (var k = 0; k < COMPONENT_KEYS.length; k++) {
            var compKey = COMPONENT_KEYS[k];
            var compState = s.components[compKey];
            if (!compState.module) continue;

            var section = renderComponentSection(compKey, compState, s.collapsed[compKey]);
            slotContentEl.appendChild(section);
            renderedCount++;
        }

        if (renderedCount === 0) {
            slotContentEl.innerHTML = '<p class="text-muted">No modules loaded in this slot</p>';
        }
    }

    // ------------------------------------------------------------------
    // Custom Module Web UI (iframe)
    // ------------------------------------------------------------------

    function renderCustomUI(s) {
        var url = s.customUI.url;

        // Create iframe container.
        var container = document.createElement("div");
        container.className = "custom-ui-container";

        var iframe = document.createElement("iframe");
        iframe.className = "custom-ui-iframe";
        iframe.src = url;
        iframe.sandbox = "allow-scripts allow-same-origin";
        iframe.setAttribute("title", "Custom Module UI");

        container.appendChild(iframe);
        slotContentEl.appendChild(container);

        // Also render FX sections below the iframe if any are loaded.
        for (var k = 0; k < COMPONENT_KEYS.length; k++) {
            var compKey = COMPONENT_KEYS[k];
            if (compKey === "synth") continue; // synth is replaced by iframe
            var compState = s.components[compKey];
            if (!compState.module) continue;
            var section = renderComponentSection(compKey, compState, s.collapsed[compKey]);
            slotContentEl.appendChild(section);
        }

        customUIIframe = iframe;
        customUISubscribed = false;
    }

    // Listen for postMessage from custom UI iframes.
    window.addEventListener("message", function (e) {
        if (!customUIIframe) return;
        // Only accept messages from our iframe.
        if (e.source !== customUIIframe.contentWindow) return;

        var msg = e.data;
        if (!msg || !msg.type) return;

        switch (msg.type) {
            case "getParam":
                handleIframeGetParam(msg);
                break;
            case "setParam":
                handleIframeSetParam(msg);
                break;
            case "subscribe":
                customUISubscribed = true;
                break;
            case "getHierarchy":
                handleIframeGetHierarchy(msg);
                break;
            case "getChainParams":
                handleIframeGetChainParams(msg);
                break;
        }
    });

    function handleIframeGetParam(msg) {
        if (!msg.key || !msg.id) return;
        var slot = activeSlot;
        if (typeof slot !== "number") return;

        // The key is like "synth:cutoff" — send via WebSocket get_param
        // (not currently supported by the WS protocol, so read from local state).
        var parts = splitPrefix(msg.key);
        var compState = slots[slot].components[parts.comp];
        var value = compState ? (compState.params[msg.key] || "") : "";

        postToIframe({ type: "paramResult", id: msg.id, value: value });
    }

    function handleIframeSetParam(msg) {
        if (!msg.key) return;
        var slot = activeSlot;
        if (typeof slot !== "number") return;

        var parts = splitPrefix(msg.key);
        send({
            type: "set_param",
            slot: slot,
            key: msg.key,
            value: String(msg.value)
        });
    }

    function handleIframeGetHierarchy(msg) {
        var slot = activeSlot;
        if (typeof slot !== "number") return;
        var compState = slots[slot].components.synth;
        postToIframe({
            type: "hierarchy",
            id: msg.id || null,
            data: compState ? compState.hierarchy : null
        });
    }

    function handleIframeGetChainParams(msg) {
        var slot = activeSlot;
        if (typeof slot !== "number") return;
        var compState = slots[slot].components.synth;
        postToIframe({
            type: "chainParams",
            id: msg.id || null,
            data: compState ? compState.chainParams : null
        });
    }

    function postToIframe(msg) {
        if (customUIIframe && customUIIframe.contentWindow) {
            customUIIframe.contentWindow.postMessage(msg, "*");
        }
    }

    function renderMasterFx() {
        slotTitleEl.textContent = "Master FX";

        var hasAnyModule = false;
        var hasAnyData = false;
        for (var i = 0; i < MASTER_FX_KEYS.length; i++) {
            var comp = masterFx.components[MASTER_FX_KEYS[i]];
            if (comp.module) hasAnyModule = true;
            if (comp.hierarchy || comp.chainParams || Object.keys(comp.params).length > 0) {
                hasAnyData = true;
            }
        }

        if (!hasAnyData && !hasAnyModule) {
            if (waitingForData || !isConnected) {
                debugEl.innerHTML = '<div class="loading-state"><span class="loading-spinner"></span> Loading Master FX...</div>';
            } else {
                debugEl.innerHTML = '<p class="text-muted">No effects loaded in Master FX</p>';
            }
            return;
        }

        if (!hasAnyModule) {
            slotContentEl.innerHTML = '<p class="text-muted">No effects loaded in Master FX</p>';
            return;
        }

        var renderedCount = 0;
        for (var k = 0; k < MASTER_FX_KEYS.length; k++) {
            var compKey = MASTER_FX_KEYS[k];
            var compState = masterFx.components[compKey];
            if (!compState.module) continue;

            var section = renderMasterFxSection(compKey, compState, masterFx.collapsed[compKey]);
            slotContentEl.appendChild(section);
            renderedCount++;
        }

        if (renderedCount === 0) {
            slotContentEl.innerHTML = '<p class="text-muted">No effects loaded in Master FX</p>';
        }
    }

    // ------------------------------------------------------------------
    // Master FX section rendering
    // ------------------------------------------------------------------

    function renderMasterFxSection(compKey, compState, isCollapsed) {
        var section = document.createElement("div");
        section.className = "component-section";
        section.setAttribute("data-component", compKey);

        // Header bar
        var header = document.createElement("div");
        header.className = "component-header" + (isCollapsed ? " collapsed" : "");
        var arrow = document.createElement("span");
        arrow.className = "component-toggle";
        arrow.textContent = isCollapsed ? "\u25B6" : "\u25BC";
        var title = document.createElement("span");
        title.className = "component-title";
        var displayLabel = MASTER_FX_LABELS[compKey] || compKey;
        var moduleName = compState.module || "";
        title.textContent = displayLabel + (moduleName ? " - " + moduleName : "");
        header.appendChild(arrow);
        header.appendChild(title);
        header.onclick = function () {
            masterFx.collapsed[compKey] = !masterFx.collapsed[compKey];
            renderSlot();
        };
        section.appendChild(header);

        if (isCollapsed) return section;

        // Body
        var body = document.createElement("div");
        body.className = "component-body";

        // Resolve auto-navigation
        resolveCompLevel(compState);

        // Breadcrumb
        if (compState.hierarchy && compState.hierarchy.levels && compState.navStack.length > 1) {
            var bcEl = document.createElement("nav");
            bcEl.className = "slot-breadcrumb";
            renderBreadcrumb(compState, compKey, bcEl);
            body.appendChild(bcEl);
        }

        var level = getCompCurrentLevel(compState);
        if (!level) {
            var noLevel = document.createElement("p");
            noLevel.className = "text-muted";
            noLevel.textContent = "No parameters available";
            body.appendChild(noLevel);
            section.appendChild(body);
            return section;
        }

        // Preset browser
        if (level.list_param && level.count_param) {
            var presetEl = renderPresetBrowser(level, compKey, compState);
            if (presetEl) body.appendChild(presetEl);
        }

        // Knob row
        var knobs = level.knobs || [];
        if (knobs.length > 0) {
            var knobRow = document.createElement("div");
            knobRow.className = "knob-row";
            for (var i = 0; i < knobs.length && i < 8; i++) {
                var knobKey = knobs[i];
                var meta = findParamMeta(compState, knobKey);
                var value = getMasterFxParamValue(compState, compKey, knobKey);
                var knobEl = renderKnob(compKey, compState, knobKey, meta, value);
                knobRow.appendChild(knobEl);
            }
            body.appendChild(knobRow);
        }

        // Param list
        var params = level.params || [];
        if (params.length > 0) {
            var paramList = document.createElement("div");
            paramList.className = "param-list";
            for (var j = 0; j < params.length; j++) {
                var item = renderParamItem(params[j], compKey, compState);
                if (item) paramList.appendChild(item);
            }
            body.appendChild(paramList);
        }

        // Empty state
        if (knobs.length === 0 && params.length === 0 && !level.list_param) {
            var empty = document.createElement("p");
            empty.className = "text-muted";
            empty.textContent = "No parameters on this level";
            body.appendChild(empty);
        }

        section.appendChild(body);
        return section;
    }

    /** Get param value from master FX component state. */
    function getMasterFxParamValue(compState, compPrefix, key) {
        var v = compState.params[compPrefix + ":" + key];
        if (v !== undefined) return v;
        return compState.params[key];
    }

    function updateMasterFxParamValues(changedParams) {
        if (activeSlot !== "master") return;
        if (!changedParams) { renderSlot(); return; }

        var needsFullRender = false;

        for (var fullKey in changedParams) {
            var value = changedParams[fullKey];
            var parts = splitMasterFxPrefix(fullKey);
            if (!parts) continue;
            var compState = masterFx.components[parts.comp];
            if (!compState) continue;

            // Skip if user is dragging this knob
            if (dragging && dragging.component === parts.comp && dragging.key === parts.key && dragging.slot === "master") continue;

            // Try to update knob visual (animated for smooth transitions)
            var knobContainer = slotContentEl.querySelector('[data-param-key="' + fullKey + '"]');
            if (knobContainer) {
                updateKnobVisualAnimated(fullKey, parseFloat(value));
            }

            // Try to update param row control
            var paramRow = slotContentEl.querySelector('[data-param-row="' + fullKey + '"]');
            if (paramRow) {
                var input = paramRow.querySelector('[data-param-input="' + fullKey + '"]');
                if (input) {
                    if (input.tagName === "SELECT") {
                        input.value = String(value);
                    } else if (input.type === "range") {
                        input.value = value;
                    }
                }
                var valDisplay = paramRow.querySelector('[data-param-value="' + fullKey + '"]');
                if (valDisplay) {
                    var meta = findParamMeta(compState, parts.key);
                    valDisplay.textContent = formatValue(value, meta);
                }
            }

            // Update preset browser if relevant
            var presetBrowser = slotContentEl.querySelector('[data-preset-browser="' + parts.comp + '"]');
            if (presetBrowser) {
                var level = getCompCurrentLevel(compState);
                if (level && (parts.key === level.list_param ||
                              parts.key === level.name_param ||
                              parts.key === level.count_param)) {
                    needsFullRender = true;
                }
            }
        }

        if (needsFullRender) renderSlot();
    }

    // ------------------------------------------------------------------
    // Status indicator
    // ------------------------------------------------------------------

    function setStatus(connected) {
        if (connected) {
            statusEl.innerHTML = '<span class="status-dot connected"></span> Connected';
            statusEl.className = "remote-ui-status connected";
        } else {
            var label = "Reconnecting...";
            if (reconnectAttempts > 0) {
                label = "Reconnecting... (attempt " + reconnectAttempts + ")";
            }
            statusEl.innerHTML = '<span class="status-dot disconnected"></span> ' + label;
            statusEl.className = "remote-ui-status disconnected";
        }
    }

    // ------------------------------------------------------------------
    // Init
    // ------------------------------------------------------------------

    connect();
})();
