// JV-880 Module UI

let frame = 0;

globalThis.init = function() {
    clear_screen();
    print(0, 0, "JV-880", 1);
    print(0, 12, "Roland Synth", 1);
    print(0, 36, "Play pads!", 1);
};

globalThis.tick = function() {
    frame++;
    if (frame % 60 === 0) {
        const buf = host_module_get_param("buffer_fill");
        print(0, 52, "buf:" + (buf || "?") + "   ", 1);
    }
};

globalThis.onMidiMessageInternal = function(data) {
    // Pass through - DSP handles MIDI
};

globalThis.onMidiMessageExternal = function(data) {
    // External MIDI goes directly to DSP
};
