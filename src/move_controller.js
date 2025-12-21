import * as std from "std";
import * as cn from "./constants.mjs";

const notes = [...Array(128).keys()];
const colours = [...Array(128).keys()];

const itemNames = [
    "Empty",
    "Master",
    "Bass Drum",
    "BD",
    "Aco BD",
    "Snare",
    "Aco Snre",
    "Ele Snre",
    "Sd Stick",
    "Stick",
    "Hihat",
    "HH",
    "HH Open",
    "HH Clsd",
    "HH Ped",
    "Toms",
    "Hi Tom",
    "Mid Tom",
    "Low Tom",
    "HF Tom",
    "LF Tom",
    "Cymbals",
    "Rides",
    "Ride",
    "Ride 1",
    "Ride 2",
    "Ride Bell",
    "Crash Cym",
    "Crash",
    "Crash 1",
    "Crash 2",
    "Splash",
    "China",
    "Cowbell",
    "Bus",
    "Shift",
    "Menu",
    "Back",
    "Capture",
    "Undo",
    "Down",
    "Up",
    "Left",
    "Right",
    "Loop",
    "Copy",
    "Play",
    "Rec",
    "Mute",
    "Record",
    "Delete",
    "Rec",
    "Row1",
    "Row2",
    "Row3",
    "Row4",
    "Main",
    "Dial",
    "Spare"
];

let bank = 0;
const knobBanksDef = {
    3: "Main",
    14: "Dial",
    40: "Row4",
    41: "Row3",
    42: "Row2",
    43: "Row1",
    49: "Shift",
    50: "Menu",
    51: "Back",
    52: "Capture",
    54: "Down",
    55: "Up",
    56: "Undo",
    58: "Loop",
    60: "Copy",
    62: "Left",
    63: "Right",
    71: "Empty",
    72: "Empty",
    73: "Empty",
    74: "Empty",
    75: "Empty",
    76: "Empty",
    77: "Empty",
    78: "Empty",
    79: "Master",
    85: "Play",
    86: "Rec",
    88: "Mute",
    118: "Record",
    119: "Delete"
};
const knobs = JSON.parse(JSON.stringify(knobBanksDef));
var knobBanks = [];
knobBanks[bank] = knobs;

const padBanksDef = {  // Blank init => PadMidiNote: [SentMidiNote, Colour, "Name"]
    68: [0, cn.LightGrey, "Empty"],
    69: [0, cn.LightGrey, "Empty"],
    70: [0, cn.LightGrey, "Empty"],
    71: [0, cn.LightGrey, "Empty"],
    72: [0, cn.LightGrey, "Empty"],
    73: [0, cn.LightGrey, "Empty"],
    74: [0, cn.LightGrey, "Empty"],
    75: [0, cn.LightGrey, "Empty"],
    76: [0, cn.LightGrey, "Empty"],
    77: [0, cn.LightGrey, "Empty"],
    78: [0, cn.LightGrey, "Empty"],
    79: [0, cn.LightGrey, "Empty"],
    80: [0, cn.LightGrey, "Empty"],
    81: [0, cn.LightGrey, "Empty"],
    82: [0, cn.LightGrey, "Empty"],
    83: [0, cn.LightGrey, "Empty"],
    84: [0, cn.LightGrey, "Empty"],
    85: [0, cn.LightGrey, "Empty"],
    86: [0, cn.LightGrey, "Empty"],
    87: [0, cn.LightGrey, "Empty"],
    88: [0, cn.LightGrey, "Empty"],
    89: [0, cn.LightGrey, "Empty"],
    90: [0, cn.LightGrey, "Empty"],
    91: [0, cn.LightGrey, "Empty"],
    92: [0, cn.LightGrey, "Empty"],
    93: [0, cn.LightGrey, "Empty"],
    94: [0, cn.LightGrey, "Empty"],
    95: [0, cn.LightGrey, "Empty"],
    96: [0, cn.LightGrey, "Empty"],
    97: [0, cn.LightGrey, "Empty"],
    98: [0, cn.LightGrey, "Empty"],
    99: [0, cn.LightGrey, "Empty"]
};
const pads = JSON.parse(JSON.stringify(padBanksDef));
var padBanks = [];
padBanks[bank] = pads;

let paramName = {
    0: "MIDI Note",
    1: "Colour",
    2: "Name",
    3: ""
};

let appName = "MOVE CONTROLS";
let editMode = false;
let lastPad = 0;
let lastCC = 0;
let ccOrPadParam = 0;
let editModeOpt = 0;
var editParam;
let line1 = "";
let line2 = "";
let line3 = "";
let line4 = "";
var displayPrev;
let midiInt = move_midi_internal_send;


function clearLEDS() {
    let i = 0;
    while (i < 127) {
        midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, i, cn.Black]);
        midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, i, cn.Black]);
        i++;
    }
}

function fillPads(pads) {
    let i = cn.MovePad1;

    while (pads[i]) {
        let pad = pads[i];
        midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, i, pad[1]]);
        i++;
    }
}

function updateDisplay() {
    clear_screen();
    print(0, 0, line1, 1);
    print(0, 17, line2, 1);
    print(0, 33, line3, 1);
    print(0, 50, line4, 1);
}

function display(l1 = " ", l2 = " ", l3 = " ", l4 = " ", temp = false) {
    line1 = l1;
    line2 = l2;
    line3 = l3;
    line4 = l4;
    if (!temp) {
        displayPrev = [l1, l2, l3, l4];
    }
}

function loadConfig() {
    var f, fname = "config.json";
    let config = std.loadFile(fname);

    if (config) {
        f = std.loadFile(fname);
        let configBanks = std.parseExtJSON(f);
        padBanks = configBanks[0];
        knobBanks = configBanks[1];
    }

    fillPads(padBanks[bank]);
}


function saveConfig() {
    var f, fname = "config.json";

    f = std.open(fname, "w");
    f.puts(JSON.stringify([padBanks, knobBanks]));
    f.close();

    let str = std.loadFile(fname);
    console.log(`Config Content: ${str}`);
}

globalThis.onMidiMessageExternal = function (data) {
    if (
        data[0] === cn.MidiClock || 
        data[0] === cn.MidiChAftertouch ||
        data[0] === cn.MidiPolyAftertouch ||
        data[0] === cn.MidiSysexStart||
        data[0] === cn.MidiSysexEnd
    ) {
        // ignoring...
        return;
    }

    console.log(`onMidiMessageExternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);

    midiInt([0 << 4 | (data[0] / 16), data[0], data[1], data[2]]);
};

globalThis.onMidiMessageInternal = function (data) {
    if (
        data[0] === cn.MidiClock || 
        data[0] === cn.MidiChAftertouch ||
        data[0] === cn.MidiPolyAftertouch ||
        data[0] === cn.MidiSysexStart||
        data[0] === cn.MidiSysexEnd
    ) {
        // ignoring...
        return;
    }

    console.log(`onMidiMessageInternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);

    let isNote = data[0] === cn.MidiNoteOn || data[0] === cn.MidiNoteOff;
    let isNoteOn = data[0] === cn.MidiNoteOn;
    let isNoteOff = data[0] === cn.MidiNoteOff;
    let isCC = data[0] === cn.MidiCC;

    if (isNote) {
        let note = data[1];
        let velocity = data[2];

        if (note === cn.MoveMainTouch) {
            return;
        } else if (note >= cn.MoveStep1 && note <= cn.MoveStep16 && velocity === 127) {
            // change banks (16) via step buttons

            // clear previous bank (step) and light new bank (step) led
            midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, (bank + 16), cn.Black]);
            midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, note, cn.White]);

            // load bank data or if empty add default template
            bank = note - 16; // convert note number to bank(step)
            if (!knobBanks[bank]) {
                knobBanks[bank] = JSON.parse(JSON.stringify(knobBanksDef));
            }
            if (!padBanks[bank]) {
                padBanks[bank] = JSON.parse(JSON.stringify(padBanksDef));
            }

            fillPads(padBanks[bank]);
            display(appName, `Bank => ${bank + 1}`);

        } else if (note >= cn.MoveKnob1Touch && note <= cn.MoveMasterTouch && velocity === 127) {
            // show paramater on knob touch

            let ccFromNote = note + 71;
            display(appName, `CC ${ccFromNote}`, knobBanks[bank][ccFromNote], `Note ${note}`, true);
        } else if (note >= cn.MoveKnob1Touch && note <= cn.MoveMasterTouch && velocity === 0) {
            // return to previous display content on touch release

            display(displayPrev[0], displayPrev[1], displayPrev[2], displayPrev[3]);
        } else  if (note >= cn.MovePad1 && note <= cn.MovePad32) {
            // all pads
            let pad = padBanks[bank][note];

            // send modified MIDI note
            move_midi_external_send([2 << 4 | (data[0] / 16), data[0], pad[0], velocity]);

            if (isNoteOn) {
                if (!editMode) {
                    midiInt([0 << 4 | (data[0] / 16), data[0], note, cn.White]);
                    display(
                        appName,
                        `Note ${pad[0]} ${cn.midiNotes[pad[0]]}`,
                        `Name ${pad[2]}`,
                        `Velocity ${velocity}`
                    );
                    return;
                } else if (editMode) {
                    display(
                        `EDIT ${paramName[editModeOpt]}`,
                        `Name ${pad[2]}`,
                        `Colour ${pad[1]}`,
                        `Note ${pad[0]}`
                    );

                    // colour row for edit mode
                    midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, editModeOpt+cn.MoveRow4, cn.Red]);
                }
            } else if (isNoteOff) {
                if (editMode) {
                    if (lastCC != 0) {  // black out previous CC control led
                        midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, lastCC, cn.Black]);
                        lastCC = 0;
                    }
                    if (lastPad === 0) {
                        lastPad = note;  // first time pressing pad
                    }
                    if (lastPad != note) {   // pressing different pad to last time
                        midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, lastPad, padBanks[bank][lastPad][1]]);
                        lastPad = note;
                    }
                    midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, note, cn.White]);
                    midiInt([0 << 4 | (0x98 / 16), 0x98, note, padBanks[bank][lastPad][1]]);
                } else if (!editMode) {
                    midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, note, pad[1]]);
                }
            }

            move_midi_external_send([2 << 4 | (data[0] / 16), data[0], data[1], data[2]]);
        }
    }

    if (isCC) {
        let ccNumber = data[1];
        let value = data[2];
        let valueText = "";
        let knobName = knobBanks[bank][ccNumber];

        if (!editMode) {
            if (
                ccNumber >= cn.MoveKnob1 &&
                ccNumber <= cn.MoveMaster ||
                ccNumber === 14
            ) {
                // add +++ and --- indicators to knobs

                if (value === 1 ) {
                    value = 127;
                    valueText = "+++"; 
                } else {
                    value = 1;
                    valueText = "---";
                }

            } else if (ccNumber === cn.MoveRecord && value === 127) {
                // enter edit mode

                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveCapture, cn.Black]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveRecord, cn.Black]);
                midiInt([0 << 4 | (0xBA / 16), 0xBA, cn.MoveRecord, cn.Red]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, editModeOpt+cn.MoveRow4, cn.Red]);
                display(appName, "EDIT MODE", "Select Pad or", "Knob to edit");
                console.log("Entered EDIT mode");
                editMode = true;
                return;

            } else if (ccNumber === cn.MoveRecord && value === 0) {
                return;  // ignore 
            } else if (ccNumber === cn.MoveCapture) {
                exit();  // exit to Move interface
            } else {
                // maybe find use for the other buttons
            }

            // send midi externally and display details on display
            move_midi_external_send([2 << 4 | (data[0] / 16), data[0], data[1], data[2]]);
            display(appName, `CC ${ccNumber} => ${valueText}`, knobName);

        } else if (editMode) {  // in edit mode
            let currentKnob = knobBanks[bank][lastCC];
            let currentPad = padBanks[bank][lastPad];
            if (
                ccNumber === cn.MoveRecord && value === 0 ||
                ccNumber === cn.MoveMainButton && value === 0
            ) {
                return; // ignore buttons
            } else if (ccNumber === cn.MoveRecord && value === 127) {
                // exit editing mode

                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveRecord, cn.Black]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveRow1, cn.Black]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveRow2, cn.Black]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveRow3, cn.Black]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveRow4, cn.Black]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveRecord, cn.Blue]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveCapture, cn.White]);
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, lastCC, cn.Black]);

                fillPads(padBanks[bank]);
                editMode = false;
                lastCC = 0;
                lastPad = 0;
                ccOrPadParam = 0;
                console.log("Exited EDIT mode");
                display(appName);
                saveConfig();

            } else if (ccNumber >= cn.MoveRow4 && ccNumber <= cn.MoveRow1) {
                // change edit parameter selection (note, colour, name)

                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, editModeOpt+cn.MoveRow4, cn.Black]);
                editModeOpt = ccNumber - cn.MoveRow4;
                midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, ccNumber, cn.Red]);
                display(`EDIT ${paramName[editModeOpt]}`);

            } else if (ccNumber === cn.MoveMainKnob && lastCC != 0) {
                // edit knob settings

                if (value === 127 && ccOrPadParam === 0) {
                    ccOrPadParam = itemNames.length - 1;
                } else if (value === 127) {
                    ccOrPadParam--;
                } else if (value === 1 && ccOrPadParam === itemNames.length - 1) {
                    ccOrPadParam = 0;
                } else if (value === 1) {
                    ccOrPadParam++;
                }
                display(
                    `EDIT CC${lastCC} Name`,
                    `Cur: ${currentKnob}`,
                    `New: ${itemNames[ccOrPadParam]}`
                );
            } else if (ccNumber === cn.MoveMainButton && lastCC != 0 && value === 127) {
                // save knob changes to knob bank

                knobBanks[bank][lastCC] = itemNames[ccOrPadParam];
                display(
                    `EDIT CC${lastCC} Name`,
                    `Now: ${knobBanks[bank][lastCC]}`
                );
            } else if (ccNumber === cn.MoveMainKnob && lastPad != 0) {
                // edit pad settings

                if (editModeOpt === 0) {
                    editParam = notes;
                } else if (editModeOpt === 1) {
                    editParam = colours;
                } else if (editModeOpt === 2) {
                    editParam = itemNames;
                }

                if (value === 127 && ccOrPadParam === 0) {
                    ccOrPadParam = editParam.length - 1;
                } else if (value === 127) {
                    ccOrPadParam--;
                } else if (value === 1 && ccOrPadParam === editParam.length - 1) {
                    ccOrPadParam = 0;
                } else if (value === 1) {
                    ccOrPadParam++;
                }

                if (editModeOpt === 1) {
                    midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, lastPad, editParam[ccOrPadParam]]);
                }

                let l4 = "";
                if (editModeOpt === 0) {
                    l4 = cn.midiNotes[ccOrPadParam];
                } else if (editModeOpt === 1) {
                    l4 = cn.colourNames[ccOrPadParam];
                } else {
                    l4 = "";
                }

                display(
                    `EDIT ${paramName[editModeOpt]}`,
                    `Cur: ${currentPad[editModeOpt]}`,
                    `New: ${editParam[ccOrPadParam]}`,
                    l4
                );
            } else if (ccNumber === cn.MoveMainButton && lastPad != 0 && value === 127) {
                // save pad changes to pad bank

                if (editModeOpt === 0 || editModeOpt === 1) {
                    padBanks[bank][lastPad][editModeOpt] = ccOrPadParam;
                } else {
                    padBanks[bank][lastPad][editModeOpt] = itemNames[ccOrPadParam];
                }

                display(
                    `EDIT ${paramName[editModeOpt]}`,
                    `Now: ${currentPad[editModeOpt]}`
                );
                midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, lastPad, cn.White]);
                midiInt([0 << 4 | (0x98 / 16), 0x98, lastPad, padBanks[bank][lastPad][1]]);
            } else if (ccNumber >= cn.MoveMainButton && ccNumber <= cn.MoveDelete) {
                // flashing led control
                if (lastPad != 0) {
                    midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, lastPad, currentPad[1]]);
                    lastPad = 0;
                }
                if (lastCC != 0) {
                    midiInt([0 << 4 | (0xB9 / 16), 0xB9, lastCC, cn.White]);
                }
                if (lastCC != ccNumber) {
                    midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, lastCC, cn.Black]);
                }

                lastCC = ccNumber;

                display(`EDIT CC Name`, `Now: ${knobBanks[bank][lastCC]}`, `CC ${lastCC}`);
            }
        }
    }
};

globalThis.init = function () {
    console.log("======================================");
    console.log("Custom Ableton Move controller started");
    console.log("======================================");

    display(appName);

    clearLEDS();

    // led for Capture (exit)
    midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveCapture, cn.White]);
    // led for Record (edit)
    midiInt([0 << 4 | (cn.MidiCC / 16), cn.MidiCC, cn.MoveRecord, cn.Blue]);
    // led for first bank
    midiInt([0 << 4 | (cn.MidiNoteOn / 16), cn.MidiNoteOn, cn.MoveStep1, cn.White]);

    loadConfig();
};

globalThis.tick = function(deltaTime) {
    updateDisplay();
};
