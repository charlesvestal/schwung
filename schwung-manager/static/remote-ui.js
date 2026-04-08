/* ================================================================
   Remote UI - WebSocket client for shadow slot parameter control
   ================================================================ */

(function () {
    "use strict";

    // Per-slot cached state.
    var slots = [
        { hierarchy: null, chainParams: null, params: {}, navStack: ["root"] },
        { hierarchy: null, chainParams: null, params: {}, navStack: ["root"] },
        { hierarchy: null, chainParams: null, params: {}, navStack: ["root"] },
        { hierarchy: null, chainParams: null, params: {}, navStack: ["root"] }
    ];

    var activeSlot = 0;
    var ws = null;
    var reconnectTimer = null;

    // Knob drag state.
    var dragging = null; // { key, startY, startValue, min, max, step, type }
    var sendThrottleTimer = null;
    var SEND_INTERVAL = 33; // ~30Hz

    // DOM references.
    var statusEl = document.getElementById("remote-ui-status");
    var slotTitleEl = document.getElementById("slot-title");
    var breadcrumbEl = document.getElementById("slot-breadcrumb");
    var knobRowEl = document.getElementById("knob-row");
    var paramListEl = document.getElementById("param-list");
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
            setStatus(true);
            subscribe(activeSlot);
        };

        ws.onmessage = function (evt) {
            var msg;
            try {
                msg = JSON.parse(evt.data);
            } catch (e) {
                return;
            }
            dispatch(msg);
        };

        ws.onclose = function () {
            setStatus(false);
            scheduleReconnect();
        };

        ws.onerror = function () {};
    }

    function scheduleReconnect() {
        if (reconnectTimer) return;
        reconnectTimer = setTimeout(function () {
            reconnectTimer = null;
            connect();
        }, 2000);
    }

    function send(obj) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify(obj));
        }
    }

    function subscribe(slot) {
        send({ type: "subscribe", slot: slot });
    }

    function unsubscribe(slot) {
        send({ type: "unsubscribe", slot: slot });
    }

    // ------------------------------------------------------------------
    // Message dispatch
    // ------------------------------------------------------------------

    function dispatch(msg) {
        var slot = msg.slot;
        if (slot < 0 || slot > 3) return;

        switch (msg.type) {
            case "hierarchy":
                slots[slot].hierarchy = msg.data;
                slots[slot].navStack = ["root"];
                if (slot === activeSlot) renderSlot();
                break;

            case "chain_params":
                slots[slot].chainParams = msg.data;
                if (slot === activeSlot) renderSlot();
                break;

            case "param_update":
                if (msg.params) {
                    var p = slots[slot].params;
                    for (var key in msg.params) {
                        p[key] = msg.params[key];
                    }
                }
                if (slot === activeSlot) updateParamValues(slot, msg.params);
                break;

            default:
                break;
        }
    }

    // ------------------------------------------------------------------
    // Slot switching
    // ------------------------------------------------------------------

    function switchSlot(n) {
        if (n === activeSlot) return;
        unsubscribe(activeSlot);
        activeSlot = n;

        tabButtons.forEach(function (btn) {
            var s = parseInt(btn.getAttribute("data-slot"), 10);
            if (s === n) {
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

    /** Look up chain_params metadata for a param key. */
    function findParamMeta(slot, key) {
        var cp = slots[slot].chainParams;
        if (!cp) return null;
        for (var i = 0; i < cp.length; i++) {
            if (cp[i].key === key) return cp[i];
        }
        return null;
    }

    /** Get param value from slot state (with synth: prefix). */
    function getParamValue(slot, key) {
        var v = slots[slot].params["synth:" + key];
        if (v !== undefined) return v;
        return slots[slot].params[key];
    }

    /** Get the current hierarchy level object. */
    function getCurrentLevel(slot) {
        var s = slots[slot];
        if (!s.hierarchy || !s.hierarchy.levels) return null;
        var stack = s.navStack;
        var levelName = stack[stack.length - 1];
        return s.hierarchy.levels[levelName] || null;
    }

    /** Resolve children: if a level has "children", auto-navigate. */
    function resolveLevel(slot) {
        var s = slots[slot];
        if (!s.hierarchy || !s.hierarchy.levels) return;
        var maxDepth = 10;
        while (maxDepth-- > 0) {
            var levelName = s.navStack[s.navStack.length - 1];
            var level = s.hierarchy.levels[levelName];
            if (!level || !level.children) break;
            // Auto-navigate to child
            var child = level.children;
            if (s.hierarchy.levels[child]) {
                s.navStack.push(child);
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
    var ARC_START_DEG = 225;  // 7 o'clock (bottom-left)
    var ARC_END_DEG = 135;    // 5 o'clock (bottom-right)
    var ARC_SWEEP_DEG = 270;  // degrees of rotation from min to max

    function degToRad(d) { return d * Math.PI / 180; }

    function polarToXY(cx, cy, r, deg) {
        var rad = degToRad(deg - 90); // SVG: 0deg = top
        return { x: cx + r * Math.cos(rad), y: cy + r * Math.sin(rad) };
    }

    function describeArc(cx, cy, r, startDeg, sweepDeg) {
        // sweepDeg is the clockwise sweep amount (always positive)
        var endDeg = startDeg + sweepDeg;
        var start = polarToXY(cx, cy, r, startDeg);
        var end = polarToXY(cx, cy, r, endDeg);
        var largeArc = sweepDeg > 180 ? 1 : 0;
        return "M " + start.x + " " + start.y +
               " A " + r + " " + r + " 0 " + largeArc + " 1 " + end.x + " " + end.y;
    }

    /** Returns the normalized 0..1 position for a value given its metadata. */
    function valueToNorm(val, meta) {
        var min = (meta && meta.min !== undefined) ? meta.min : 0;
        var max = (meta && meta.max !== undefined) ? meta.max : 1;
        if (max === min) return 0;
        var t = (val - min) / (max - min);
        return Math.max(0, Math.min(1, t));
    }

    /** Returns the absolute angle (may be > 360) for an indicator at normalized position t. */
    function normToAngle(t) {
        return ARC_START_DEG + t * ARC_SWEEP_DEG;
    }

    function createKnobSVG(key, meta, value) {
        var cx = KNOB_SIZE / 2;
        var cy = KNOB_SIZE / 2;
        var numVal = parseFloat(value);
        if (isNaN(numVal)) numVal = meta ? (meta.min || 0) : 0;

        var t = valueToNorm(numVal, meta);
        var indicatorAngle = normToAngle(t); // absolute degrees (may be > 360)
        var indicator = polarToXY(cx, cy, KNOB_RADIUS - 4, indicatorAngle);

        // Background arc (full sweep)
        var bgPath = describeArc(cx, cy, KNOB_RADIUS, ARC_START_DEG, ARC_SWEEP_DEG);

        // Value arc - sweep from 0 to current position
        var valSweep = t * ARC_SWEEP_DEG;
        var valPath = "";
        if (valSweep > 1) {
            valPath = describeArc(cx, cy, KNOB_RADIUS, ARC_START_DEG, valSweep);
        }

        var svg = '<svg class="knob-svg" width="' + KNOB_SIZE + '" height="' + KNOB_SIZE + '" viewBox="0 0 ' + KNOB_SIZE + ' ' + KNOB_SIZE + '">';
        // Background arc track
        svg += '<path d="' + bgPath + '" fill="none" stroke="#444" stroke-width="4" stroke-linecap="round"/>';
        // Value arc
        if (valPath) {
            svg += '<path class="knob-value-arc" d="' + valPath + '" fill="none" stroke="#e8a84c" stroke-width="4" stroke-linecap="round"/>';
        }
        // Center dot
        svg += '<circle cx="' + cx + '" cy="' + cy + '" r="3" fill="#888"/>';
        // Indicator line
        svg += '<line class="knob-indicator" x1="' + cx + '" y1="' + cy + '" x2="' + indicator.x + '" y2="' + indicator.y + '" stroke="#e8a84c" stroke-width="2" stroke-linecap="round"/>';
        svg += '</svg>';
        return svg;
    }

    function renderKnob(key, meta, value) {
        var container = document.createElement("div");
        container.className = "knob-container";
        container.setAttribute("data-param-key", key);

        var label = (meta && meta.name) ? meta.name : key;
        var displayVal = formatValue(value !== undefined ? value : (meta ? (meta.min || 0) : 0), meta);

        container.innerHTML =
            createKnobSVG(key, meta, value !== undefined ? value : (meta ? (meta.min || 0) : 0)) +
            '<div class="knob-label">' + escapeHtml(label) + '</div>' +
            '<div class="knob-value" data-knob-value="' + escapeHtml(key) + '">' + escapeHtml(displayVal) + '</div>';

        // Drag handling
        var svgEl = container; // drag target is the whole container
        function onPointerDown(e) {
            e.preventDefault();
            var numVal = parseFloat(getParamValue(activeSlot, key));
            if (isNaN(numVal)) numVal = meta ? (meta.min || 0) : 0;
            var clientY = e.touches ? e.touches[0].clientY : e.clientY;
            dragging = {
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

        svgEl.addEventListener("mousedown", onPointerDown);
        svgEl.addEventListener("touchstart", onPointerDown, { passive: false });

        return container;
    }

    function onPointerMove(e) {
        if (!dragging) return;
        e.preventDefault();
        var clientY = e.touches ? e.touches[0].clientY : e.clientY;
        var dy = dragging.startY - clientY; // up = positive
        var range = dragging.max - dragging.min;
        var sensitivity = range / 150; // 150px for full range
        var newVal = dragging.startValue + dy * sensitivity;

        // Snap to step
        if (dragging.step > 0) {
            newVal = Math.round(newVal / dragging.step) * dragging.step;
        }
        newVal = clampValue(newVal, dragging);
        if (dragging.type === "int") newVal = Math.round(newVal);

        // Update local state optimistically
        var prefixedKey = "synth:" + dragging.key;
        slots[dragging.slot].params[prefixedKey] = String(newVal);

        // Update knob visually
        updateKnobVisual(dragging.key, newVal);

        // Throttled send
        if (!sendThrottleTimer) {
            sendThrottleTimer = setTimeout(function () {
                sendThrottleTimer = null;
            }, SEND_INTERVAL);
            sendParamValue(dragging.slot, dragging.key, newVal);
        }
    }

    function onPointerUp(e) {
        if (!dragging) return;
        // Send final value
        var prefixedKey = "synth:" + dragging.key;
        var finalVal = slots[dragging.slot].params[prefixedKey];
        sendParamValue(dragging.slot, dragging.key, parseFloat(finalVal));
        dragging = null;
        document.removeEventListener("mousemove", onPointerMove);
        document.removeEventListener("mouseup", onPointerUp);
        document.removeEventListener("touchmove", onPointerMove);
        document.removeEventListener("touchend", onPointerUp);
    }

    function updateKnobVisual(key, value) {
        var container = knobRowEl.querySelector('[data-param-key="' + key + '"]');
        if (!container) return;
        var meta = findParamMeta(activeSlot, key);
        var svgParent = container.querySelector(".knob-svg");
        if (svgParent) {
            // Replace SVG
            var tempDiv = document.createElement("div");
            tempDiv.innerHTML = createKnobSVG(key, meta, value);
            container.replaceChild(tempDiv.firstChild, svgParent);
        }
        var valEl = container.querySelector('[data-knob-value="' + key + '"]');
        if (valEl) {
            valEl.textContent = formatValue(value, meta);
        }
    }

    // ------------------------------------------------------------------
    // Send param
    // ------------------------------------------------------------------

    function sendParamValue(slot, key, value) {
        send({
            type: "set_param",
            slot: slot,
            key: "synth:" + key,
            value: String(value)
        });
    }

    // ------------------------------------------------------------------
    // Preset browser
    // ------------------------------------------------------------------

    function renderPresetBrowser(level, slot) {
        if (!level.list_param || !level.count_param) return null;

        var countVal = getParamValue(slot, level.count_param);
        var currentVal = getParamValue(slot, level.list_param);
        var nameVal = level.name_param ? getParamValue(slot, level.name_param) : null;

        var count = parseInt(countVal, 10) || 0;
        var current = parseInt(currentVal, 10) || 0;

        var container = document.createElement("div");
        container.className = "preset-browser";
        container.setAttribute("data-preset-browser", "true");

        var prevBtn = document.createElement("button");
        prevBtn.className = "preset-nav-btn";
        prevBtn.textContent = "\u25C0";
        prevBtn.disabled = current <= 0;
        prevBtn.onclick = function () {
            if (current > 0) {
                sendParamValue(slot, level.list_param, current - 1);
            }
        };

        var nextBtn = document.createElement("button");
        nextBtn.className = "preset-nav-btn";
        nextBtn.textContent = "\u25B6";
        nextBtn.disabled = current >= count - 1;
        nextBtn.onclick = function () {
            if (current < count - 1) {
                sendParamValue(slot, level.list_param, current + 1);
            }
        };

        var nameSpan = document.createElement("span");
        nameSpan.className = "preset-name";
        nameSpan.setAttribute("data-preset-name", "true");
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

    function renderParamItem(entry, slot) {
        // Normalize entry
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
                slots[activeSlot].navStack.push(navLevel);
                resolveLevel(activeSlot);
                renderSlot();
            };
            return row;
        }

        // Editable param
        if (!key) return null;
        var meta = findParamMeta(slot, key);
        var value = getParamValue(slot, key);
        if (!label && meta) label = meta.name;
        if (!label) label = key;

        var row = document.createElement("div");
        row.className = "param-row";
        row.setAttribute("data-param-row", key);

        var labelSpan = document.createElement("span");
        labelSpan.className = "param-label";
        labelSpan.textContent = label;
        row.appendChild(labelSpan);

        var controlWrap = document.createElement("div");
        controlWrap.className = "param-control";

        if (!meta) {
            // No metadata - show value as text
            var valSpan = document.createElement("span");
            valSpan.className = "param-value-text";
            valSpan.setAttribute("data-param-value", key);
            valSpan.textContent = value !== undefined ? String(value) : "?";
            controlWrap.appendChild(valSpan);
        } else if (meta.type === "enum" && meta.options) {
            // Dropdown
            var select = document.createElement("select");
            select.className = "param-select";
            select.setAttribute("data-param-input", key);
            for (var i = 0; i < meta.options.length; i++) {
                var opt = document.createElement("option");
                opt.value = String(i);
                opt.textContent = meta.options[i];
                if (String(i) === String(value)) opt.selected = true;
                select.appendChild(opt);
            }
            select.onchange = function () {
                var v = select.value;
                slots[slot].params["synth:" + key] = v;
                sendParamValue(slot, key, parseInt(v, 10));
            };
            controlWrap.appendChild(select);
        } else if (meta.type === "float" || meta.type === "int") {
            // Slider
            var slider = document.createElement("input");
            slider.type = "range";
            slider.className = "param-slider";
            slider.setAttribute("data-param-input", key);
            slider.min = meta.min !== undefined ? meta.min : 0;
            slider.max = meta.max !== undefined ? meta.max : (meta.type === "int" ? 127 : 1);
            slider.step = meta.step || (meta.type === "int" ? 1 : 0.01);
            slider.value = value !== undefined ? value : slider.min;

            var valDisplay = document.createElement("span");
            valDisplay.className = "param-slider-value";
            valDisplay.setAttribute("data-param-value", key);
            valDisplay.textContent = formatValue(value !== undefined ? value : slider.min, meta);

            slider.oninput = function () {
                var v = meta.type === "int" ? parseInt(slider.value, 10) : parseFloat(slider.value);
                slots[slot].params["synth:" + key] = String(v);
                valDisplay.textContent = formatValue(v, meta);
                // Throttled send
                if (!sendThrottleTimer) {
                    sendThrottleTimer = setTimeout(function () {
                        sendThrottleTimer = null;
                    }, SEND_INTERVAL);
                    sendParamValue(slot, key, v);
                }
            };
            slider.onchange = function () {
                var v = meta.type === "int" ? parseInt(slider.value, 10) : parseFloat(slider.value);
                sendParamValue(slot, key, v);
            };

            controlWrap.appendChild(slider);
            controlWrap.appendChild(valDisplay);
        } else {
            // Unknown type
            var valSpan2 = document.createElement("span");
            valSpan2.className = "param-value-text";
            valSpan2.setAttribute("data-param-value", key);
            valSpan2.textContent = value !== undefined ? String(value) : "?";
            controlWrap.appendChild(valSpan2);
        }

        row.appendChild(controlWrap);
        return row;
    }

    // ------------------------------------------------------------------
    // Breadcrumb
    // ------------------------------------------------------------------

    function renderBreadcrumb(slot) {
        var s = slots[slot];
        breadcrumbEl.innerHTML = "";
        if (!s.hierarchy || !s.hierarchy.levels) return;

        var stack = s.navStack;
        for (var i = 0; i < stack.length; i++) {
            if (i > 0) {
                var sep = document.createElement("span");
                sep.className = "breadcrumb-sep";
                sep.textContent = " > ";
                breadcrumbEl.appendChild(sep);
            }

            var levelName = stack[i];
            var levelObj = s.hierarchy.levels[levelName];
            var label = (levelObj && levelObj.label) ? levelObj.label : levelName;

            if (i < stack.length - 1) {
                // Clickable - navigate back
                var link = document.createElement("a");
                link.textContent = label;
                link.setAttribute("data-nav-index", String(i));
                link.onclick = (function (idx) {
                    return function () {
                        slots[activeSlot].navStack = slots[activeSlot].navStack.slice(0, idx + 1);
                        renderSlot();
                    };
                })(i);
                breadcrumbEl.appendChild(link);
            } else {
                // Current level - not clickable
                var span = document.createElement("span");
                span.className = "breadcrumb-current";
                span.textContent = label;
                breadcrumbEl.appendChild(span);
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
            // Extract unprefixed key
            var key = prefixedKey;
            if (key.indexOf("synth:") === 0) key = key.substring(6);

            // Skip if user is dragging this knob
            if (dragging && dragging.key === key && dragging.slot === slot) continue;

            // Try to update knob visual
            var knobContainer = knobRowEl.querySelector('[data-param-key="' + key + '"]');
            if (knobContainer) {
                updateKnobVisual(key, parseFloat(value));
            }

            // Try to update param row control
            var paramRow = paramListEl.querySelector('[data-param-row="' + key + '"]');
            if (paramRow) {
                var input = paramRow.querySelector('[data-param-input="' + key + '"]');
                if (input) {
                    if (input.tagName === "SELECT") {
                        input.value = String(value);
                    } else if (input.type === "range") {
                        input.value = value;
                    }
                }
                var valDisplay = paramRow.querySelector('[data-param-value="' + key + '"]');
                if (valDisplay) {
                    var meta = findParamMeta(slot, key);
                    valDisplay.textContent = formatValue(value, meta);
                }
            }

            // Update preset browser if relevant
            var presetBrowser = paramListEl.parentElement.querySelector('[data-preset-browser]');
            if (presetBrowser) {
                var level = getCurrentLevel(slot);
                if (level && (prefixedKey === "synth:" + level.list_param ||
                              prefixedKey === "synth:" + level.name_param ||
                              prefixedKey === "synth:" + level.count_param)) {
                    needsFullRender = true;
                }
            }
        }

        if (needsFullRender) renderSlot();
    }

    // ------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------

    function renderSlot() {
        var s = slots[activeSlot];
        slotTitleEl.textContent = "Slot " + (activeSlot + 1);

        knobRowEl.innerHTML = "";
        paramListEl.innerHTML = "";
        debugEl.innerHTML = "";

        // No data at all
        if (!s.hierarchy && !s.chainParams && Object.keys(s.params).length === 0) {
            debugEl.innerHTML = '<p class="text-muted">Waiting for data...</p>';
            breadcrumbEl.innerHTML = "";
            return;
        }

        // No hierarchy - empty slot
        if (!s.hierarchy) {
            breadcrumbEl.innerHTML = "";
            paramListEl.innerHTML = '<p class="text-muted">No module loaded in this slot</p>';
            return;
        }

        // Resolve auto-navigation (children)
        resolveLevel(activeSlot);

        // Breadcrumb
        renderBreadcrumb(activeSlot);

        var level = getCurrentLevel(activeSlot);
        if (!level) {
            paramListEl.innerHTML = '<p class="text-muted">Unknown hierarchy level</p>';
            return;
        }

        // Preset browser
        if (level.list_param && level.count_param) {
            var presetEl = renderPresetBrowser(level, activeSlot);
            if (presetEl) {
                paramListEl.appendChild(presetEl);
            }
        }

        // Knob row
        var knobs = level.knobs || [];
        for (var i = 0; i < knobs.length && i < 8; i++) {
            var knobKey = knobs[i];
            var meta = findParamMeta(activeSlot, knobKey);
            var value = getParamValue(activeSlot, knobKey);
            var knobEl = renderKnob(knobKey, meta, value);
            knobRowEl.appendChild(knobEl);
        }

        // Param list
        var params = level.params || [];
        for (var j = 0; j < params.length; j++) {
            var item = renderParamItem(params[j], activeSlot);
            if (item) paramListEl.appendChild(item);
        }

        // Empty state messages
        if (knobs.length === 0 && params.length === 0 && !level.list_param) {
            paramListEl.innerHTML = '<p class="text-muted">No parameters on this level</p>';
        }

        if (!s.chainParams && knobs.length > 0) {
            var notice = document.createElement("p");
            notice.className = "text-muted";
            notice.textContent = "No param metadata available - controls may not display correctly";
            paramListEl.insertBefore(notice, paramListEl.firstChild);
        }
    }

    // ------------------------------------------------------------------
    // Status indicator
    // ------------------------------------------------------------------

    function setStatus(connected) {
        if (connected) {
            statusEl.textContent = "Connected";
            statusEl.className = "remote-ui-status connected";
        } else {
            statusEl.textContent = "Disconnected";
            statusEl.className = "remote-ui-status disconnected";
        }
    }

    // ------------------------------------------------------------------
    // Init
    // ------------------------------------------------------------------

    connect();
})();
