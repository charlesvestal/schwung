/* ================================================================
   Remote UI - WebSocket client for shadow slot parameter control
   ================================================================ */

(function () {
    "use strict";

    // Per-slot cached state.
    var slots = [
        { hierarchy: null, chainParams: null, params: {} },
        { hierarchy: null, chainParams: null, params: {} },
        { hierarchy: null, chainParams: null, params: {} },
        { hierarchy: null, chainParams: null, params: {} }
    ];

    var activeSlot = 0;
    var ws = null;
    var reconnectTimer = null;

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
            // Subscribe to the active slot on connect.
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

        ws.onerror = function () {
            // onclose will fire after onerror.
        };
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
                if (slot === activeSlot) renderSlot();
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

        // Unsubscribe from old slot.
        unsubscribe(activeSlot);

        activeSlot = n;

        // Update tab UI.
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

        // Subscribe to new slot.
        subscribe(n);

        // Re-render with cached data.
        renderSlot();
    }

    // Expose to global for onclick handlers.
    window.switchSlot = switchSlot;

    // ------------------------------------------------------------------
    // Rendering (placeholder - Task 4 will add real knob/param UI)
    // ------------------------------------------------------------------

    function renderSlot() {
        var s = slots[activeSlot];
        slotTitleEl.textContent = "Slot " + (activeSlot + 1);
        breadcrumbEl.textContent = "";

        // Clear knob/param areas (Task 4 will populate these).
        knobRowEl.innerHTML = "";
        paramListEl.innerHTML = "";

        // Debug / placeholder display.
        if (!s.hierarchy && !s.chainParams && Object.keys(s.params).length === 0) {
            debugEl.innerHTML = '<p class="text-muted">Waiting for data...</p>';
            return;
        }

        var html = "";

        if (s.hierarchy) {
            html += '<div class="debug-section">';
            html += "<h3>Hierarchy</h3><ul>";
            var levels = s.hierarchy.levels || {};
            for (var levelName in levels) {
                var level = levels[levelName];
                var label = level.label || levelName;
                var knobCount = (level.knobs || []).length;
                var paramCount = (level.params || []).length;
                html += "<li><strong>" + escapeHtml(levelName) + "</strong>: " +
                    escapeHtml(label) + " (" + knobCount + " knobs, " + paramCount + " params)</li>";
            }
            html += "</ul></div>";
        }

        if (s.chainParams && s.chainParams.length > 0) {
            html += '<div class="debug-section">';
            html += "<h3>Chain Params (" + s.chainParams.length + ")</h3><ul>";
            for (var i = 0; i < s.chainParams.length; i++) {
                var cp = s.chainParams[i];
                html += "<li>" + escapeHtml(cp.key) + " (" + escapeHtml(cp.type || "?") + ")</li>";
            }
            html += "</ul></div>";
        }

        var paramKeys = Object.keys(s.params);
        if (paramKeys.length > 0) {
            html += '<div class="debug-section">';
            html += "<h3>Params (" + paramKeys.length + ")</h3><ul>";
            paramKeys.sort();
            for (var j = 0; j < paramKeys.length; j++) {
                html += "<li>" + escapeHtml(paramKeys[j]) + " = " + escapeHtml(String(s.params[paramKeys[j]])) + "</li>";
            }
            html += "</ul></div>";
        }

        debugEl.innerHTML = html;
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
    // Helpers
    // ------------------------------------------------------------------

    function escapeHtml(str) {
        var div = document.createElement("div");
        div.appendChild(document.createTextNode(str));
        return div.innerHTML;
    }

    // ------------------------------------------------------------------
    // Init
    // ------------------------------------------------------------------

    connect();
})();
