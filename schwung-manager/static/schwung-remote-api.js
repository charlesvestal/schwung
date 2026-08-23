/* ================================================================
   schwung-remote-api.js
   Include this in your module's web_ui.html to communicate with
   the Schwung Remote UI host page.

   Two transports, selected automatically:
     - Iframe mode (default): embedded in the manager's Remote UI page,
       talks to the parent frame via postMessage.
     - Standalone mode: when the page is popped out into its own window
       (opened with ?schwungStandalone=1&slot=N, i.e. a top-level window
       with no manager parent), it opens its own /ws/remote-ui WebSocket
       and talks to the device directly — independent of the manager tab.

   The public API is identical in both modes:
     <script src="/static/schwung-remote-api.js"></script>
     <script>
       schwungRemote.onParamChange(function(params) {
         console.log("params changed", params);
       });
       schwungRemote.getParam("synth:cutoff").then(function(val) {
         console.log("cutoff =", val);
       });
       schwungRemote.setParam("synth:cutoff", "0.75");
     </script>
   ================================================================ */

(function () {
    "use strict";

    // Standalone (popped-out) when this page is top-level rather than embedded
    // in the manager's iframe. The explicit query flag is the reliable signal
    // (and carries the slot); window.top === window.self is the fallback.
    var query = new URLSearchParams(window.location.search);
    var standalone = query.get("schwungStandalone") === "1" || window.top === window.self;
    // Which chain component this page drives. The manager appends it to the
    // iframe URL, so an audio FX panel asks for fx1/fx2 metadata rather than
    // the synth's. Defaults to synth for pages predating the flag.
    var component = query.get("component") || "synth";

    if (!standalone) {
        // ----------------------------------------------------------------
        // Iframe mode — bridge to the manager parent page via postMessage.
        // ----------------------------------------------------------------
        var reqCounter = 0;

        function makeId() {
            return "req_" + (++reqCounter) + "_" + Date.now();
        }

        function requestFromParent(msg) {
            return new Promise(function (resolve) {
                var id = makeId();
                msg.id = id;

                function handler(e) {
                    if (e.data && e.data.id === id) {
                        window.removeEventListener("message", handler);
                        resolve(e.data);
                    }
                }

                window.addEventListener("message", handler);
                window.parent.postMessage(msg, "*");
            });
        }

        // Tell the parent how tall this page actually is, so the iframe can be
        // sized to its content instead of to a guess. Every embedded module
        // gets this without opting in — the alternative is each module posting
        // its own height, which means the ones that never do stay cropped.
        //
        // documentElement.scrollHeight, not body's: the body may be shorter
        // than a floated/absolutely-positioned child, and cropping is exactly
        // what we are fixing. Rounded up, because a fractional height the
        // parent floors reintroduces a 1px scrollbar, which changes the width,
        // which can change the height — a loop that never settles.
        var lastHeight = 0;
        function reportHeight() {
            var h = Math.ceil(document.documentElement.scrollHeight);
            if (!h || Math.abs(h - lastHeight) < 2) return;
            lastHeight = h;
            window.parent.postMessage({ type: "height", height: h }, "*");
        }
        if (typeof ResizeObserver === "function") {
            new ResizeObserver(reportHeight).observe(document.documentElement);
        } else {
            window.addEventListener("resize", reportHeight);
        }
        window.addEventListener("load", reportHeight);
        /* Fonts and late layout land after load; a few cheap re-checks cost
         * nothing and save a permanently short frame. */
        setTimeout(reportHeight, 100);
        setTimeout(reportHeight, 600);

        window.schwungRemote = {
            /** Chain component this page drives ("synth", "fx1", …). */
            component: component,
            /**
             * Get the current value of a parameter.
             * @param {string} key - Parameter key, e.g. "synth:cutoff"
             * @returns {Promise<string>} The parameter value
             */
            getParam: function (key) {
                return requestFromParent({ type: "getParam", key: key }).then(function (resp) {
                    return resp.value;
                });
            },

            /**
             * Set a parameter value.
             * @param {string} key - Parameter key, e.g. "synth:cutoff"
             * @param {string|number} value - New value
             */
            setParam: function (key, value) {
                window.parent.postMessage({ type: "setParam", key: key, value: String(value) }, "*");
            },

            /**
             * Register a callback for parameter value changes.
             * @param {function} callback - Called with an object of {key: value} pairs
             */
            onParamChange: function (callback) {
                window.addEventListener("message", function (e) {
                    if (e.data && e.data.type === "paramUpdate") {
                        callback(e.data.params);
                    }
                });
                // Tell the parent to start forwarding param updates.
                window.parent.postMessage({ type: "subscribe" }, "*");
            },

            /**
             * Get the synth module's UI hierarchy.
             * @returns {Promise<object>} The hierarchy data
             */
            getHierarchy: function () {
                return requestFromParent({ type: "getHierarchy" }).then(function (resp) {
                    return resp.data;
                });
            },

            /**
             * Get the synth module's chain_params metadata.
             * @returns {Promise<Array>} The chain_params array
             */
            getChainParams: function () {
                return requestFromParent({ type: "getChainParams" }).then(function (resp) {
                    return resp.data;
                });
            },

            /**
             * Ask the parent to re-fetch the current view's params from the
             * device and re-push them. Used by modules whose device-side state
             * changes without a notify (e.g. overtake tools polling for the
             * playhead / external edits). Params arrive via onParamChange.
             */
            resubscribe: function () {
                window.parent.postMessage({ type: "resubscribe" }, "*");
            }
        };
        return;
    }

    // --------------------------------------------------------------------
    // Standalone mode — own WebSocket connection to /ws/remote-ui.
    // --------------------------------------------------------------------
    var slot = parseInt(query.get("slot"), 10);
    if (isNaN(slot)) slot = 0;
    // Overtake-tool pop-out: use the tool channel (subscribe_tool / refetch_tool)
    // and route sets on slot 0 (the manager dispatches by the overtake_dsp: key
    // prefix), rather than a per-slot subscribe.
    var isTool = query.get("tool") === "1";

    var wsUrl = (window.location.protocol === "https:" ? "wss://" : "ws://") +
        window.location.host + "/ws/remote-ui";

    var cache = {};               // key -> last known value (mirrors parent cache)
    var listeners = [];           // onParamChange callbacks
    var hierarchyData = null, hierarchyWaiters = [];
    var chainParamsData = null, chainParamsWaiters = [];
    var ws = null;
    var reconnectDelay = 1000;    // exponential backoff, capped
    var RECONNECT_MAX = 10000;

    function copyCache() {
        var out = {};
        for (var k in cache) {
            if (Object.prototype.hasOwnProperty.call(cache, k)) out[k] = cache[k];
        }
        return out;
    }

    function emit(params) {
        for (var i = 0; i < listeners.length; i++) {
            try { listeners[i](params); } catch (e) { /* keep other listeners alive */ }
        }
    }

    function resolveWaiters(arr, val) {
        while (arr.length) arr.shift()(val);
    }

    function handleMessage(msg) {
        switch (msg.type) {
            case "param_update":
                if (msg.params) {
                    for (var k in msg.params) {
                        if (Object.prototype.hasOwnProperty.call(msg.params, k)) {
                            cache[k] = msg.params[k];
                        }
                    }
                    emit(msg.params);
                }
                break;
            case "hierarchy":
                // Take the metadata for the component this page drives.
                if (msg.component === component) {
                    hierarchyData = msg.data;
                    resolveWaiters(hierarchyWaiters, hierarchyData);
                }
                break;
            case "chain_params":
                if (msg.component === component) {
                    chainParamsData = msg.data;
                    resolveWaiters(chainParamsWaiters, chainParamsData);
                }
                break;
            // slot_info / custom_ui / error are not needed by the module UI.
        }
    }

    function connect() {
        ws = new WebSocket(wsUrl);

        ws.onopen = function () {
            reconnectDelay = 1000;
            if (isTool) {
                ws.send(JSON.stringify({ type: "subscribe_tool" }));
            } else {
                ws.send(JSON.stringify({ type: "subscribe", slot: slot }));
            }
        };

        ws.onmessage = function (e) {
            var msg;
            try { msg = JSON.parse(e.data); } catch (err) { return; }
            if (msg && msg.type) handleMessage(msg);
        };

        ws.onclose = function () {
            ws = null;
            setTimeout(connect, reconnectDelay);
            reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_MAX);
        };

        ws.onerror = function () {
            try { if (ws) ws.close(); } catch (e) { /* ignore */ }
        };
    }

    connect();

    window.schwungRemote = {
        /** Chain component this page drives ("synth", "fx1", …). */
        component: component,
        getParam: function (key) {
            // Resolve from the local cache. Values stream in via param_update
            // shortly after subscribe, so early reads may be undefined — the
            // same best-effort contract as the iframe path's seed/onParamChange.
            return Promise.resolve(cache[key]);
        },

        setParam: function (key, value) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({
                    type: "set_param",
                    slot: isTool ? 0 : slot,
                    key: key,
                    value: String(value)
                }));
            }
        },

        onParamChange: function (callback) {
            listeners.push(callback);
            // Replay whatever we already have so a late subscriber is seeded.
            if (Object.keys(cache).length) {
                try { callback(copyCache()); } catch (e) { /* ignore */ }
            }
        },

        getHierarchy: function () {
            if (hierarchyData !== null) return Promise.resolve(hierarchyData);
            return new Promise(function (resolve) { hierarchyWaiters.push(resolve); });
        },

        getChainParams: function () {
            if (chainParamsData !== null) return Promise.resolve(chainParamsData);
            return new Promise(function (resolve) { chainParamsWaiters.push(resolve); });
        },

        resubscribe: function () {
            // Re-request a fresh push over our own socket. Tool pop-out uses the
            // params-only refetch (a full subscribe would re-send custom_ui).
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify(isTool ? { type: "refetch_tool" }
                                              : { type: "subscribe", slot: slot }));
            }
        }
    };
})();
