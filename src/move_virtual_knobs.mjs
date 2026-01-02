import { clamp } from "./math_helpers.mjs";
import { display } from "./move_display.mjs";
import * as std from "std";

const knobLEDs = {}; // LED color cache
const synthwaveColorSweep = [104, 105, 20, 21, 23, 26, 25];
const roseColorSweep = [124, 35, 23, 26, 25];
const neutralColorSweep = [0, 124, 123, 120];
const rainbowColorSweep = [33, 16, 15, 14, 11, 8, 3, 2];
const black = 0;
const white = 120;
const red = 127;

let itemIndex = 0;

// just some example names to work with, more can be added
const itemNames = [
    "Main",
    "Prm1",
    "Prm2",
    "Prm3",
    "Prm4",
    "Prm5",
    "Prm6",
    "Prm7",
    "Prm8",
    "Track 1",
    "Track 2",
    "Track 3",
    "Track 4",
    "Track 5",
    "Track 6",
    "Track 7",
    "Track 8",
    "Master Mix",
    "ModFX",
    "Delay",
    "Reverb",
    "Limiter",
    "InL Mix",
    "InL ModFX",
    "InL Delay",
    "InL Reverb",
    "InR Mix",
    "InR ModFX",
    "InR Delay",
    "InR Reverb",
    "USB Mix",
    "USB ModFX",
    "USB Delay",
    "USB Reverb",
    "DJ Filter",
    "OTT",
    "00 Size",
    "00 Mult",
    "00 Warp",
    "00 Scan",
    "00 Timbre",
    "00 Color",
    "00 Degrade",
    "00 Redux",
    "00 Start",
    "00 Loop St",
    "00 Length",
    "00 Detune",
    "00 Degrade",
    "00 MOD1",
    "00 MOD2",
    "00 MOD3",
    "00 MOD4",
    "00 Shift",
    "00 Swarm",
    "00 Width",
    "00 SubOsc",
    "00 Cutoff",
    "00 Reso",
    "00 Amp",
    "00 Pan",
    "00 Dry",
    "00 ModFX",
    "00 Delay",
    "00 Reverb",
    "Spare",
];

const banksDef = [
    {
        bank: 0,
        ccMap: [ // CC71 - CC79
            [71, 71], [72, 72], [73, 73], [74, 74], [75, 75], [76, 76], [77, 77], [78, 78], [79, 79]
        ],
        colorSweep: rainbowColorSweep,
        knobs: [0, 0, 0, 0, 0, 0, 0, 0, 0],
        names: ["Prm1", "Prm2","Prm3","Prm4","Prm5","Prm6","Prm7","Prm8","Prm9"],
        led: 17
    },
    {
        bank: 1,
        ccMap: [ // CC14 - CC22
            [71, 14], [72, 15], [73, 16], [74, 17], [75, 18], [76, 19], [77, 20], [78, 21], [79, 22]
        ],
        colorSweep: rainbowColorSweep,
        knobs: [0, 0, 0, 0, 0, 0, 0, 0, 0],
        names: ["Prm1", "Prm2","Prm3","Prm4","Prm5","Prm6","Prm7","Prm8","Prm9"],
        led: 19
    },
    {
        bank: 2, 
        ccMap: [ // CC23 - CC31
            [71, 23], [72, 24], [73, 25], [74, 26], [75, 27], [76, 28], [77, 29], [78, 30], [79, 31]
        ],
        colorSweep: rainbowColorSweep,
        knobs: [0, 0, 0, 0, 0, 0, 0, 0, 0],
        names: ["Prm1", "Prm2","Prm3","Prm4","Prm5","Prm6","Prm7","Prm8","Prm9"],
        led: 21
    },
    {
        bank: 3,
        ccMap: [ // CC35 - CC43
            [71, 35], [72, 36], [73, 37], [74, 38], [75, 39], [76, 40], [77, 41], [78, 42], [79, 43]
        ],
        colorSweep: rainbowColorSweep,
        knobs: [0, 0, 0, 0, 0, 0, 0, 0, 0],
        names: ["Prm1", "Prm2","Prm3","Prm4","Prm5","Prm6","Prm7","Prm8","Prm9"],
        led: 23
    },
    {
        bank: 4,
        ccMap: [ // CC44 - CC52
            [71, 44], [72, 45], [73, 46], [74, 47], [75, 48], [76, 49], [77, 50], [78, 51], [79, 52]
        ],
        colorSweep: rainbowColorSweep,
        knobs: [0, 0, 0, 0, 0, 0, 0, 0, 0],
        names: ["Prm1", "Prm2","Prm3","Prm4","Prm5","Prm6","Prm7","Prm8","Prm9"],
        led: 25
    },
    {
        bank: 5, 
        ccMap: [ // CC53 - CC61
            [71, 53], [72, 54], [73, 55], [74, 56], [75, 57], [76, 58], [77, 59], [78, 60], [79, 61]
        ],
        colorSweep: rainbowColorSweep,
        knobs: [0, 0, 0, 0, 0, 0, 0, 0, 0],
        names: ["Prm1", "Prm2","Prm3","Prm4","Prm5","Prm6","Prm7","Prm8","Prm9"],
        led: 27
    }, 
    {
        bank: 6,
        ccMap: [ // CC102 - CC110
            [71, 102], [72, 103], [73, 104], [74, 105], [75, 106], [76, 107], [77, 108], [78, 109], [79, 110]
        ],
        colorSweep: rainbowColorSweep,
        knobs: [0, 0, 0, 0, 0, 0, 0, 0, 0],
        names: ["Prm1", "Prm2","Prm3","Prm4","Prm5","Prm6","Prm7","Prm8","Prm9"],
        led: 29
    },
    {
        bank: 7,
        ccMap: [ // CC111 - CC119
            [71, 111], [72, 112], [73, 113], [74, 114], [75, 115], [76, 116], [77, 117], [78, 118], [79, 119]
        ],
        colorSweep: rainbowColorSweep,
        knobs: [0, 0, 0, 0, 0, 0, 0, 0, 0],
        names: ["Prm1", "Prm2","Prm3","Prm4","Prm5","Prm6","Prm7","Prm8","Prm9"],
        led: 31
    },
    {
        bank: 8,
        ccMap: [ // CC120 - CC128
            [71, 120], [72, 121], [73, 122], [74, 124], [75, 125], [76, 126], [77, 127], [78, 128], [79, 129]
        ],
        colorSweep: neutralColorSweep,
        knobs: [112, 112, 112, 112, 112, 112, 112, 112, 112],
        names: ["Track 1", "Track 2","Track 3","Track 4","Track 5","Track 6","Track 7","Track 8","Main"],
        led: 118 // record audio LED for main volume indicator
    }
];

let currentSave = 0;
let currentBank = 8;
let saveBanks = [];
saveBanks[currentSave] = JSON.parse(JSON.stringify(banksDef));
let banks = saveBanks[currentSave];
let bank = banks[currentBank];
var knobs;
var names;


export function updateConfig() {
    if (!saveBanks[currentSave]) {
        saveBanks[currentSave] = JSON.parse(JSON.stringify(banksDef));
    }
    banks = saveBanks[currentSave];
    bank = banks[currentBank];
    knobs = bank.knobs;
    names = bank.names;

    // set knob LEDs
    setKnobLed(71, knobs[0]);
    setKnobLed(72, knobs[1]);
    setKnobLed(73, knobs[2]);
    setKnobLed(74, knobs[3]);
    setKnobLed(75, knobs[4]);
    setKnobLed(76, knobs[5]);
    setKnobLed(77, knobs[6]);
    setKnobLed(78, knobs[7]);
    setKnobLed(79, knobs[8]);

    display("Move Anything", `Save ${currentSave} Bank ${currentBank}`);
}

export function loadConfig() {
    var f, fname = "m8config.json";
    let config = std.loadFile(fname);

    if (config) {
        f = std.loadFile(fname);
        let configBanks = std.parseExtJSON(f);
        saveBanks = configBanks;
    }
}

function saveConfig() {
    var f, fname = "m8config.json";

    f = std.open(fname, "w");
    f.puts(JSON.stringify(saveBanks));
    f.close();

    let str = std.loadFile(fname);
    console.log(`Config Content: ${str}`);
}

function getColorForKnobValue(value = 0) {
    // const colorSweep = neutralColorSweep;
    const colorSweep = bank.colorSweep;

    const level = clamp(value, 0, 127) / 127;
    const index = Math.round(level * (colorSweep.length - 1));

    return colorSweep[index];
}

function setKnobLed(moveControlNumber, value) {
    // check if we're just setting the same color
    const color = getColorForKnobValue(value);
    if (knobLEDs[moveControlNumber] === color) return;
    if (moveControlNumber === 79) moveControlNumber = 118; // record audio led for main vol
    move_midi_internal_send([0 << 4 | 0xb, 0xb1 | 0, moveControlNumber, color]);
    knobLEDs[moveControlNumber] = color;
}

export function changeSave(index = 0) {
    // save state
    saveConfig();

    // stop flashing old saved bank LED
    move_midi_internal_send([0 << 4 | 0x9, 0x91 | 0, (currentSave*2)+17, black]);

    currentSave = index;

    // flash saved bank LED
    move_midi_internal_send([0 << 4 | 0x99/16, 0x99 | 0, (index*2)+17, red]);

    changeBank(currentBank);
}

export function changeBank(index = 0) {
    // save knob state
    banks[currentBank].knobs = knobs;
    currentBank = index;

    // turn off old bank LED
    move_midi_internal_send([0 << 4 | 0x9, 0x91 | 0, bank.led, black]);

    // toggle bank to show main volume bank (8)
    if (index === bank.bank) {
        console.log("Toggle bank");
        currentBank = 8;
    }

    updateConfig();

    // set bank LED
    move_midi_internal_send([0 << 4 | 0x9, 0x91 | 0, bank.led, white]);

}


export function handleMoveKnobs(data, shiftHeld = false, channel = 3) {
    let knob = -1;
    let ccNumber = -1;

    let moveControlNumber = data[1];
    let value = data[2];
    let mapped = new Map(bank.ccMap);

    console.log(moveControlNumber, value);

    if (data[0] != 0xb0) {
        // touch knob to get value
        if (moveControlNumber >= 0 && moveControlNumber <= 8) {
            knob = moveControlNumber;
            ccNumber = mapped.get(moveControlNumber + 71);
        } else {
            return;
        }
    }

    // moveControlNumber to knob number
    if (moveControlNumber >= 71 && moveControlNumber <= 80) {
        knob = moveControlNumber - 71;
        ccNumber = mapped.get(moveControlNumber);

        if (shiftHeld && data[0] === 0xb0) {
            if (value === 127) {
                if (itemIndex <= 0) {
                    itemIndex = itemNames.length;
                } else {
                    itemIndex -= 1;
                }
            }
            if (value === 1) {
                if (itemIndex >= itemNames.length) {
                    itemIndex = 0;
                } else {
                    itemIndex += 1;
                }
            }
            names[knob] = itemNames[itemIndex];

        }
        if (!shiftHeld && data[0] === 0xb0) {
            if (value === 127) {
                if (knobs[knob] > 0) {
                    knobs[knob] -= 1;
                }
            }

            if (value === 1) {
                if (knobs[knob] < 127) {
                    knobs[knob] += 1;
                }
            }

            console.log(`Sending CC ${ccNumber} value: ${knobs[knob]}`);

            move_midi_external_send([2 << 4 | 0xb, 0xb0 | channel, ccNumber, knobs[knob]]);

            setKnobLed(moveControlNumber, knobs[knob]);
        }
    }

    display(
        "Move Anything", 
        `Save ${currentSave} Bank ${bank.bank}`, 
        `${names[knob]}`, 
        `CC ${ccNumber} - ${knobs[knob]}`
    );

}
