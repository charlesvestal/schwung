/* ================================================================
   schwung-remote-api.js
   Include this in your module's web_ui.html to communicate with
   the Schwung Remote UI host page via postMessage.

   Usage:
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

    window.schwungRemote = {
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
        }
    };
})();
