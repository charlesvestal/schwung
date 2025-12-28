import { updateDisplay, updateOverlay, display } from "./move_display.mjs";
import * as cn from "./constants.mjs";
import * as os from "os";

let position = 1;

globalThis.onMidiMessageExternal = function (data) {
    return;
};

globalThis.onMidiMessageInternal = function (data) {
    let isNote = data[0] === cn.MidiNoteOn || data[0] === cn.MidiNoteOff;
    let isCC = data[0] === cn.MidiCC;
    let isAt = data[0] === cn.MidiPolyAftertouch;
    let moveCC = data[1];
    let value = data[2];

    if (isAt || isNote) {
        return;
    }

    console.log(`onMidiMessageInternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);

    if (isCC) {
        console.log(`Move: message:`, data);

        if (moveCC === cn.MoveMainButton && value === 127) {
            // do something
            if (position === 1) {
                exit();
                os.exec(["./control_surface_move", "move_m8_vlpp.js"], {block: false});
            } else if (position === 2) {
                exit();
                os.exec(["./control_surface_move", "move_controller.js"], {block: false});
            } else if (position === 3) {
                exit();
                os.exec(["./control_surface_move", "move_draw.js"], {block: false});
            }

        }
        if (moveCC === cn.MoveMainKnob) {
            if (value === 127) {
                if (position === 1) {
                    position = 3;
                } else {
                    position -= 1;
                }
            } else if (value === 1) {
                if (position === 3) {
                    position = 1;
                } else {
                    position += 1;
                }
            }
            updateOverlay(position);
        }
    }
};

globalThis.init = function () {
    console.log("Move control surface script staring...");

    display(
        "MoveAnything",
        "M8 LPP",
        "Controller",
        "Spare"
    );
};

globalThis.tick = function () {
    updateDisplay();
    updateOverlay(position);
};
