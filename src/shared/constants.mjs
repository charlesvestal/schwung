// See https://github.com/Ableton/push-interface/blob/main/doc/AbletonPush2MIDIDisplayInterface.asc 
// for Push2 and most the same Move internal MIDI commands


/* from https://github.com/damian-/move-anything/blob/dev/src/move_virtual_knobs.mjs
================================================================================
RGB COLOR PALETTE (INDEXED 0–127)
================================================================================

--- NEUTRALS / GREYS -----------------------------------------------------------
  0 : #000000  Black
117 : #000000  Black (dup)
124 : #1A1A1A  Dark Grey
119 : #1A1A1A  Dark Grey (dup)
118 : #595959  Light Grey
121 : #595959  Light Grey (dup)
123 : #595959  Light Grey (dup)
120 : #FFFFFF  White
122 : #FFFFFF  White (dup)

--- REDS / PINKS / MAGENTAS ----------------------------------------------------
  1 : #FF2424  Bright Red
 27 : #A63421  Rust Red
 65 : #4D0B0B  Deep Red
 66 : #1A0404  Very Dark Red
 67 : #4D1204  Brick
 20 : #8700FF  Electric Violet
 21 : #E657E3  Hot Magenta
 23 : #FF0099  Neon Pink
 24 : #A14C5F  Rose
 25 : #FF4DC4  Bright Pink
 26 : #EB8BE1  Light Magenta
104 : #0D001A  Deep Violet
105 : #4D1D4C  Muted Violet
107 : #33004D  Dark Purple
109 : #4D002E  Deep Magenta
111 : #4D242D  Dusty Rose
113 : #4D173B  Mauve
114 : #1A0814  Deep Wine
115 : #4D2D49  Dusky Mauve
116 : #1A0F18  Shadow Mauve

--- ORANGES / AMBERS / YELLOWS -------------------------------------------------
  2 : #F23A0C  Orange Red
  3 : #FF9900  Bright Orange
  4 : #A68956  Tan / Muted Orange
  5 : #EDF95A  Light Yellow
  6 : #C19D08  Ochre
  7 : #FFFF00  Vivid Yellow
 28 : #995628  Burnt Orange
 29 : #876700  Mustard
 30 : #90821F  Yellow-Green
 73 : #4D491F  Dull Yellow
 74 : #1A180A  Very Dark Yellow
 75 : #403302  Brown-Yellow
 76 : #1A1501  Deep Brown-Yellow
 77 : #4D4D00  Olive
 78 : #1A1A00  Dark Olive

--- GREENS / TEALS -------------------------------------------------------------
  8 : #56BF13  Bright Green
  9 : #2C8403  Forest Green
 10 : #246B24  Dull Green
 11 : #19FF30  Neon Green
 12 : #159573  Teal Green
 13 : #176B50  Muted Teal
 14 : #00FFFF  Cyan
 31 : #4A8700  Lime
 32 : #007F12  Deep Green
 43 : #AEFF99  Pale Green
 44 : #7CDD9F  Mint Green
 45 : #89B47D  Olive Green
 79 : #1C4007  Dull Green
 80 : #0B1A03  Very Dark Green
 81 : #113301  Dull Olive
 83 : #113311  Dark Olive Green
 85 : #0A4D0A  Dark Grass Green
 87 : #073327  Dark Teal
 89 : #104D39  Muted Sea Green
 90 : #030D0A  Deep Teal

--- CYANS / AQUAS / BLUES ------------------------------------------------------
 15 : #0074FC  Azure Blue
 16 : #274FCC  Royal Blue
 17 : #00448C  Navy
 33 : #1853B2  Blue
 46 : #80F3FF  Pale Cyan
 47 : #7ACEFC  Sky Blue
 48 : #68A1D3  Light Blue
 49 : #858FC2  Muted Blue
 50 : #BBAAF2  Lavender Blue
 93 : #00234D  Deep Blue
 95 : #0C1940  Dark Blue
 97 : #00254D  Cool Blue
 99 : #231A4D  Indigo
100 : #0C091A  Deep Indigo
101 : #251E4D  Purple-Blue
102 : #0C0A1A  Dark Indigo
125 : #0000FF  Pure Blue

--- PURPLES / VIOLETS ---------------------------------------------------------
 18 : #644AD9  Blue-Violet
 19 : #4D3FA0  Violet
 22 : #660099  Purple
 34 : #624BAD  Lilac
 35 : #733A67  Mauve
106 : #1A0A19  Deep Plum
108 : #11001A  Dark Violet
110 : #1A000F  Wine Purple
112 : #1A0C0F  Dark Rose
115 : #4D2D49  Muted Violet (duplicate family)
 20–26 : also span violet–pink boundary (see REDS)


--- PRIMARY COLORS -------------------------------------------------------------
125 : #0000FF  Blue
126 : #00FF00  Green
127 : #FF0000  Red
================================================================================
*/

// --- NEUTRALS / GREYS ---
export const Black = 0;
export const DarkGrey = 124;
export const LightGrey = 118;
export const White = 120;

// --- REDS / PINKS / MAGENTAS ---
export const BrightRed = 1;
export const RustRed = 27;
export const DeepRed = 65;
export const VeryDarkRed = 66;
export const Brick = 67 ;
export const ElectricViolet = 20 ;
export const HotMagenta = 21;
export const NeonPink = 23;
export const Rose = 24;
export const BrightPink = 25;
export const LightMagenta = 26;
export const DeepViolet = 104;
export const MutedViolet = 105;
export const DarkPurple = 107;
export const DeepMagenta = 109;
export const DustyRose = 111;
export const Mauve = 113;
export const DeepWine = 114;
export const DuskyMauve = 115;
export const ShadowMauve = 116;

// --- ORANGES / AMBERS / YELLOWS ---
export const OrangeRed = 2;
export const Bright = 3;
export const Tan = 4;
export const LightYellow = 5;
export const Ochre = 6;
export const VividYellow = 7;
export const BurntOrange = 28;
export const Mustard = 29;
export const YellowGreen = 30;
export const DullYellow = 73;
export const VeryDarkYellow = 74;
export const BrownYellow = 75;
export const DeepBrownYellow = 76;
export const Olive = 77;
export const DarkOlive = 78;

// --- GREENS / TEALS ---
export const BrightGreen = 8;
export const ForestGreen = 9;
export const DullGreen = 10;
export const NeonGreen = 11;
export const TealGreen = 12;
export const MutedTeal = 13;
export const Cyan = 14;
export const Lime = 31;
export const DeepGreen = 32;
export const PaleGreen = 43;
export const MintGreen = 44;
export const OliveGreen = 45;
export const VeryDarkGreen = 80;
export const DullOlive = 81;
export const DarkOliveGreen = 83;
export const DarkGrassGreen = 85;
export const DarkTeal = 87;
export const MutedSeaGreen = 89;
export const DeepTeal = 90;

// --- CYANS / AQUAS / BLUES ---
export const AzureBlue = 15;
export const RoyalBlue = 16;
export const Navy = 17;
export const PaleCyan = 46;
export const SkyBlue = 47;
export const LightBlue = 48;
export const MutedBlue = 49;
export const LavenderBlue = 50;
export const DeepBlue = 93;
export const DarkBlue = 95;
export const CoolBlue = 97;
export const Indigo = 99;
export const DeepBlueIndigo = 100;
export const PurpleBlue = 101;
export const DarkIndigo = 102;
export const PureBlue = 125;

// --- PURPLES / VIOLETS ---
export const BlueViolet = 18;
export const Violet = 19;
export const Purple = 22;
export const Lilac = 34;
export const DeepPlum = 106;
export const DarkViolet = 108;
export const WinePurple = 110;
export const DarkRose = 112;

// --- PRIMARY COLORS ---
export const Blue = 125;
export const Green = 126;
export const Red = 127;

export const colourNames = {  // for pads, steps and play, rec, and record leds
  0: "Black",
  1: "Bright Red",
  2: "Orange Red",
  3: "Bright Orange",
  4: "Tan / Muted Orange",
  5: "Light Yellow",
  6: "Ochre",
  7: "Vivid Yellow",
  8: "Bright Green",
  9: "Forest Green",
  10: "Dull Green",
  11: "Neon Green",
  12: "Teal Green",
  13: "Muted Teal",
  14: "Cyan",
  15: "Azure Blue",
  16: "Royal Blue",
  17: "Navy",
  18: "Blue-Violet",
  19: "Violet",
  20: "Electric Violet",
  21: "Hot Magenta",
  22: "Purple",
  23: "Neon Pink",
  24: "Rose",
  25: "Bright Pink",
  26: "Light Magenta",
  27: "Rust Red",
  28: "Burnt Orange",
  29: "Mustard",
  30: "Yellow-Green",
  31: "Lime",
  32: "Deep Green",
  33: "Blue",
  34: "Lilac",
  35: "Mauve",
  36: "",
  37: "",
  38: "",
  39: "",
  40: "",
  41: "",
  42: "",
  43: "Pale Green",
  44: "Mint Green",
  45: "Olive Green",
  46: "Pale Cyan",
  47: "Sky Blue",
  48: "Light Blue",
  49: "Muted Blue",
  50: "Lavender Blue",
  51: "",
  52: "",
  53: "",
  54: "",
  55: "",
  56: "",
  57: "",
  58: "",
  59: "",
  60: "",
  61: "",
  62: "",
  63: "",
  64: "",
  65: "Deep Red",
  66: "Very Dark Red",
  67: "Brick",
  68: "",
  69: "",
  70: "",
  71: "",
  72: "",
  73: "Dull Yellow",
  74: "Very Dark Yellow",
  75: "Brown-Yellow",
  76: "Deep Brown-Yellow",
  77: "Olive",
  78: "Dark Olive",
  79: "Dull Green",
  80: "Very Dark Green",
  81: "Dull Olive",
  82: "",
  83: "Dark Olive Green",
  84: "",
  85: "Dark Grass Green",
  86: "",
  87: "Dark Teal",
  88: "",
  89: "Muted Sea Green",
  90: "Deep Teal",
  91: "",
  92: "",
  93: "Deep Blue",
  94: "",
  95: "Dark Blue",
  96: "",
  97: "Cool Blue",
  98: "",
  99: "Indigo",
  100: "Deep Indigo",
  101: "Purple-Blue",
  102: "Dark Indigo",
  104: "Deep Violet",
  105: "Muted Violet",
  106: "Deep Plum",
  107: "Dark Purple",
  108: "Dark Violet",
  109: "Deep Magenta",
  110: "Wine Purple",
  111: "Dusty Rose",
  112: "Dark Rose",
  113: "Mauve",
  114: "Deep Wine",
  115: "Dusky Mauve",
  116: "Shadow Mauve",
  117: "Black (dup)",
  118: "Light Grey",
  119: "Dark Grey (dup)",
  120: "White",
  121: "Light Grey (dup)",
  122: "White (dup)",
  123: "Light Grey (dup)",
  124: "Dark Grey",
  125: "Blue",
  126: "Green",
  127: "Red",
  128: ""
};

export const midiNotes = {
  0: "C-2",
  1: "C#-2/Db-2",
  2: "D-2",
  3: "D#-2/Eb-2",
  4: "E-2",
  5: "F-2",
  6: "F#-2/Gb-2",
  7: "G-2",
  8: "G#-2/Ab-2",
  9: "A-2",
  10: "A#-2/Bb-2",
  11: "B-2",
  12: "C-1",
  13: "C#-1/Db-1",
  14: "D-1",
  15: "D#-1/Eb-1",
  16: "E-1",
  17: "F-1",
  18: "F#-1/Gb-1",
  19: "G-1",
  20: "G#-1/Ab-1",
  21: "A0",
  22: "A#0/Bb0",
  23: "B0",
  24: "C1",
  25: "C#1/Db1",
  26: "D1",
  27: "D#1/Eb1",
  28: "E1",
  29: "F1",
  30: "F#1/Gb1",
  31: "G1",
  32: "G#1/Ab1",
  33: "A1",
  34: "A#1/Bb1",
  35: "B1",
  36: "C2",
  37: "C#2/Db2",
  38: "D2",
  39: "D#2/Eb2",
  40: "E2",
  41: "F2",
  42: "F#2/Gb2",
  43: "G2",
  44: "G#2/Ab2",
  45: "A2",
  46: "A#2/Bb2",
  47: "B2",
  48: "C3",
  49: "C#3/Db3",
  50: "D3",
  51: "D#3/Eb3",
  52: "E3",
  53: "F3",
  54: "F#3/Gb3",
  55: "G3",
  56: "G#3/Ab3",
  57: "A3",
  58: "A#3/Bb3",
  59: "B3",
  60: "C4 midC",
  61: "C#4/Db4",
  62: "D4",
  63: "D#4/Eb4",
  64: "E4",
  65: "F4",
  66: "F#4/Gb4",
  67: "G4",
  68: "G#4/Ab4",
  69: "A4",
  70: "A#4/Bb4",
  71: "B4",
  72: "C5",
  73: "C#5/Db5",
  74: "D5",
  75: "D#5/Eb5",
  76: "E5",
  77: "F5",
  78: "F#5/Gb5",
  79: "G5",
  80: "G#5/Ab5",
  81: "A5",
  82: "A#5/Bb5",
  83: "B5",
  84: "C6",
  85: "C#6/Db6",
  86: "D6",
  87: "D#6/Eb6",
  88: "E6",
  89: "F6",
  90: "F#6/Gb6",
  91: "G6",
  92: "G#6/Ab6",
  93: "A6",
  94: "A#6/Bb6",
  95: "B6",
  96: "C7",
  97: "C#7/Db7",
  98: "D7",
  99: "D#7/Eb7",
  100: "E7",
  101: "F7",
  102: "F#7/Gb7",
  103: "G7",
  104: "G#7/Ab7",
  105: "A7",
  106: "A#7/Bb7",
  107: "B7",
  108: "C8",
  109: "C#8/Db8",
  110: "D8",
  111: "D#8/Eb8",
  112: "E8",
  113: "F8",
  114: "F#8/Gb8",
  115: "G8",
  116: "G#8/Ab8",
  117: "A8",
  118: "A#8/Bb8",
  119: "B8",
  120: "C9",
  121: "C#9/Db9",
  122: "D9",
  123: "D#9/Eb9",
  124: "E9",
  125: "F9",
  126: "F#9/Gb9",
  127: "G9"
};


// MIDI messages 
export const MidiNoteOff = 0x80;
export const MidiNoteOn = 0x90;
export const MidiPolyAftertouch = 0xA0;
export const MidiCC = 0xB0;
export const MidiPC = 0xC0;
export const MidiChAftertouch = 0xD0;
export const MidiWheel = 0xE0;
export const MidiSysexStart = 0xF0;
export const MidiSysexEnd = 0xF7;
export const MidiClock = 0xF8;

export const MidiCCOn = 0x7F;
export const MidiCCOff = 0x00;
export const MIDIChannels = Array.from({length: 16}, (x, i) => i+1);


// Internal MIDI Notes
export const MoveKnob1Touch = 0;  // on = 127, off = 0-63
export const MoveKnob2Touch = 1;
export const MoveKnob3Touch = 2;
export const MoveKnob4Touch = 3;
export const MoveKnob5Touch = 4;
export const MoveKnob6Touch = 5;
export const MoveKnob7Touch = 6;
export const MoveKnob8Touch = 7;
export const MoveMasterTouch = 8;
export const MoveMainTouch = 9;
export const MoveStep1 = 16;   // and LED
export const MoveStep2 = 17;   // and LED
export const MoveStep3 = 18;   // and LED
export const MoveStep4 = 19;   // and LED
export const MoveStep5 = 20;   // and LED
export const MoveStep6 = 21;   // and LED
export const MoveStep7 = 22;   // and LED
export const MoveStep8 = 23;   // and LED
export const MoveStep9 = 24;   // and LED
export const MoveStep10 = 25;   // and LED
export const MoveStep11 = 26;   // and LED
export const MoveStep12 = 27;   // and LED
export const MoveStep13 = 28;   // and LED
export const MoveStep14 = 29;   // and LED
export const MoveStep15 = 30;   // and LED
export const MoveStep16 = 31;   // and LED
export const MovePad1 = 68;   // and LED
// PADs 68-99 from bottom left to top right
export const MovePad32 = 99;   // and LED

// Internal MIDI CCs
export const MoveMainButton = 3;   // no LED
export const MoveMainKnob = 14;   // no LED
export const MoveStep1UI = 16;   // LED only
export const MoveStep2UI = 17;   // LED only
export const MoveStep3UI = 18;   // LED only
export const MoveStep4UI = 19;   // LED only
export const MoveStep5UI = 20;   // LED only
export const MoveStep6UI = 21;   // LED only
export const MoveStep7UI = 22;   // LED only
export const MoveStep8UI = 23;   // LED only
export const MoveStep9UI = 24;   // LED only
export const MoveStep10UI = 25;   // LED only
export const MoveStep11UI = 26;   // LED only
export const MoveStep12UI = 27;   // LED only
export const MoveStep13UI = 28;   // LED only
export const MoveStep14UI = 29;   // LED only
export const MoveStep15UI = 30;   // LED only
export const MoveStep16UI = 31;   // LED only
export const MoveRow4 = 40;   // bottom row    RGB led
export const MoveRow3 = 41;   // RGB led
export const MoveRow2 = 42;   // RGB led
export const MoveRow1 = 43;   // RGB led
export const MoveShift = 49;
export const MoveMenu = 50;
export const MoveBack = 51;
export const MoveCapture = 52;
export const MoveDown = 54;
export const MoveUp = 55;
export const MoveUndo = 56;
export const MoveLoop = 58;
export const MoveCopy = 60;
export const MoveLeft = 62;
export const MoveRight = 63;
export const MoveKnob1 = 71;   // clockwise = 1-63, counter clockwise = 64-127
export const MoveKnob2 = 72;
export const MoveKnob3 = 73;
export const MoveKnob4 = 74;
export const MoveKnob5 = 75;
export const MoveKnob6 = 76;
export const MoveKnob7 = 77;
export const MoveKnob8 = 78;
export const MoveMaster = 79;   // no LED
export const MovePlay = 85;
export const MoveRec = 86;
export const MoveMute = 88;
export const MoveMicOrAudIn = 114;   // Plug detect - MIC in = 0, Line in = 127
export const MoveSpkrOrAudOut = 115;   // Plug detect - Spkr out = 0, Line out = 127
export const MoveRecord = 118;   // RGB LED
export const MoveDelete = 119;

// Groupings
export const MovePads = Array.from({length: 32}, (x, i) => i + 68);
export const MoveSteps = Array.from({length: 16}, (x, i) => i + 16);
export const MoveTracks = [MoveRow4, MoveRow3, MoveRow2, MoveRow1];  // Track buttons 1-4
export const MoveCCButtons = [
  MoveMainButton,
  MoveBack,
  MoveMenu,
  MovePlay,
  MoveRec,
  MoveCapture,
  MoveRecord,
  MoveLoop,
  MoveMute,
  MoveDelete,
  MoveCopy,
  MoveUndo,
  MoveShift,
  MoveUp,
  MoveLeft,
  MoveRight,
  MoveDown
];
export const MoveNoteButtons = [...MoveSteps];
export const MoveRGBLeds = [
  ...MovePads,
  ...MoveSteps,
  MovePlay,
  MoveRec,
  MoveRecord
];
export const MoveWhiteLeds = [
  MoveBack,
  MoveMenu,
  MoveCapture,
  MoveLoop,
  MoveMute,
  MoveDelete,
  MoveCopy,
  MoveUndo,
  MoveShift,
  MoveUp,
  MoveLeft,
  MoveRight,
  MoveDown
];


// LED Animations
export const NoAnimation = 0x00;
export const Trans24th = 0x01;   // 24th note based on tempo
export const Trans16th = 0x02;
export const Trans8th = 0x03;
export const Trans4th = 0x04;
export const Trans2th = 0x05;
export const Pulse24th = 0x06;
export const Pulse16th = 0x07;
export const Pulse8th = 0x08;
export const Pulse4th = 0x09;
export const Pulse2th = 0x0A;
export const Blink24th = 0x0B;
export const Blink16th = 0x0C;
export const Blink8th = 0x0D;
export const Blink4th = 0x0E;
export const Blink2th = 0x0F;
