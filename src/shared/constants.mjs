// See https://github.com/Ableton/push-interface/blob/main/doc/AbletonPush2MIDIDisplayInterface.asc 
// for Push2 and most the same Move internal MIDI commands


/* Reference: https://github.com/Cycling74/rnbo.move.control (MIT)
 * See also: docs/reference/rnbo-move-control/NOTES.md, schwung_move.h

================================================================================
RGB COLOR PALETTE (INDEXED 0–127)
================================================================================

--- NEUTRALS / GREYS -----------------------------------------------------------
  0 : #000000  Black
117 : #000000  Black (dup)
118 : #595959  Light Grey
119 : #1A1A1A  Dark Grey
120 : #FFFFFF  White
121 : #595959  Light Grey (dup)
122 : #CCCCCC  White (dup)
123 : #404040  Dark Grey (dup)
124 : #141414  Dark Grey 2

--- REDS / ORANGES / YELLOWS ---------------------------------------------------
  1 : #FF4032  Bright Red              dim  65 #661914  dark  66 #210806
  2 : #800400  Orange Red              dim  67 #460300  dark  68 #280000
  3 : #C93C00  Bright Orange           dim  69 #5D1700  dark  70 #200D00
  4 : #AC1F00  Tan / Muted Orange      dim  71 #470C00  dark  72 #1C0800
  5 : #8C5018  Light Yellow            dim  73 #3B2B14  dark  74 #1C130A
  6 : #491804  Ochre                   dim  75 #250E05  dark  76 #0D0602  
  7 : #FADC3B  Vivid Yellow            dim  77 #645817  dark  78 #201C07
  8 : #FFC516  Bright Yellow           dim  79 #664E08  dark  80 #211902
  9 : #B6FF0E  Bright Lime             dim  81 #486605  dark  82 #172101

--- GREENS / TEALS -------------------------------------------------------------
 10 : #79FF18  Dull Green              dim  83 #306609  dark  84 #0F2103
 11 : #34C216  Neon Green              dim  85 #144D08  dark  86 #061902
 12 : #4F8A04  Teal Green              dim  87 #1F3701  dark  88 #0A1100
 13 : #62FF55  Muted Teal              dim  89 #276622  dark  90 #0C210B
 14 : #297D53  Cyan-Teal               dim  91 #143E29  dark  92 #081910
 15 : #269E72  Teal-Cyan               dim  93 #004D36, dark  94 #00180E

--- CYANS / AQUAS / BLUES ------------------------------------------------------
 16 : #31ADFF  Azure Blue              dim  95 #134566  dark  96 #061621
 17 : #3663FC  Royal Blue              dim  97 #152764  dark  98 #070C20
 18 : #1A34FF  Blue-Violet             dim  99 #0A1466  dark 100 #030621
 19 : #1C0CE6  Violet                  dim 101 #0B045C  dark 102 #03011D

--- PURPLES / MAGENTAS / PINKS -------------------------------------------------
 20 : #153999  Electric Violet         dim 103 #0A1C4C  dark 104 #040B1E
 21 : #3937FF  Hot Magenta             dim 105 #161666  dark 106 #070721
 22 : #5722FF  Purple                  dim 107 #220D66  dark 108 #0B0421
 23 : #972BFF  Neon Pink               dim 109 #3C1166  dark 110 #130521
 24 : #852178  Rose                    dim 111 #350D30  dark 112 #11040F
 25 : #FF1032  Bright Pink             dim 113 #660614  dark 114 #210206
 26 : #FF2BD4  Light Magenta           dim 115 #661154  dark 116 #21051B

--- SATURATION VARIANTS (27–35) ------------------------------------------------
 27 : #A63421  Rust Red
 28 : #995628  Burnt Orange
 29 : #876700  Mustard
 30 : #90821F  Yellow-Green
 31 : #4A8700  Lime
 32 : #007F12  Deep Green
 33 : #1853B2  Blue
 34 : #624BAD  Lilac
 35 : #733A67  Mauve

--- PASTELS / LIGHT TONES (36–64) ----------------------------------------------
 36 : #F8BCAF  Pale Salmon
 37 : #FF9B76  Light Orange
 38 : #FFBF5F  Light Amber
 39 : #D9AF71  Sand
 40 : #FFF480  Light Yellow 2
 41 : #BFBA69  Pale Olive
 42 : #BCCC88  Pale Lime
 43 : #AEFF99  Pale Green
 44 : #7CDD9F  Mint Green
 45 : #89B47D  Olive Green
 46 : #80F3FF  Pale Cyan
 47 : #7ACEFC  Sky Blue
 48 : #68A1D3  Light Blue
 49 : #858FC2  Muted Blue
 50 : #BBAAF2  Lavender Blue
 51 : #CDBBE4  Pale Lavender
 52 : #EF8BB0  Pale Pink
 53 : #859D8C  Pale Sea Green
 54 : #6B756E  Grey Green
 55 : #84909B  Grey Blue
 56 : #6A7075  Steel Grey
 57 : #88859D  Lavender Grey
 58 : #6C6A75  Dark Steel
 59 : #9D859C  Mauve Grey
 60 : #746A74  Warm Grey
 61 : #9C9D85  Olive Grey
 62 : #74756A  Sage Grey
 63 : #9D8484  Rose Grey
 64 : #756A6A  Brown Grey

--- PRIMARY COLORS -------------------------------------------------------------
125 : #0000FF  Blue
126 : #00FF00  Green
127 : #FF0000  Red
================================================================================
*/

// --- NEUTRALS / GREYS ---
export const Black = 0;
export const LightGrey = 118;
export const DarkGrey = 119;
export const DarkGrey2 = 124;
/* The two greys the palette lists as duplicates. They are not duplicates of
 * each other, and both sit in gaps a brightness ramp needs: #404040 between
 * DarkGrey (#1A1A1A) and LightGrey (#595959), and #CCCCCC between LightGrey
 * and White. See knob_leds.mjs WHITE_LEVELS. */
export const DarkGrey3 = 123;
export const OffWhite = 122;
export const White = 120;

// --- BRIGHT COLOURS 1–26 WITH DIM/DARK PARTNERS ---
export const BrightRed = 1;
export const DeepRed = 65;
export const VeryDarkRed = 66;
export const OrangeRed = 2;
export const DeepOrangeRed = 67;
export const VeryDarkOrangeRed = 68;
export const BrightOrange = 3;
export const BurntSienna = 69;
export const DarkBrown = 70;
export const Tan = 4;
export const DarkRust = 71;
export const DarkOrange = 72;
export const LightYellow = 5;
export const DarkYellow = 73;
export const VeryDarkYellow = 74;
export const Ochre = 6;
export const DarkBrown2 = 75;
export const DeepBrown = 76;
export const VividYellow = 7;
export const Olive = 77;
export const DarkOlive = 78;
export const BrightYellow = 8;
export const DarkYellowOlive = 79;
export const VeryDarkYellowOlive = 80;
export const BrightLime = 9;
export const DarkLime = 81;
export const VeryDarkLime = 82;
export const DullGreen = 10;
export const DarkGreen = 83;
export const VeryDarkGreen = 84;
export const NeonGreen = 11;
export const DarkGrass = 85;
export const VeryDarkGrass = 86;
export const TealGreen = 12;
export const DarkTealGreen = 87;
export const VeryDarkTealGreen = 88;
export const MutedTeal = 13;
export const DarkMutedTeal = 89;
export const VeryDarkMutedTeal = 90;
export const CyanTeal = 14;
export const DarkCyanTeal = 91;
export const VeryDarkCyanTeal = 92;
export const TealCyan = 15;
export const DeepTealCyan = 93;
export const VeryDeepTealCyan = 94;
export const AzureBlue = 16;
export const DarkAzure = 95;
export const VeryDarkAzure = 96;
export const RoyalBlue = 17;
export const DarkRoyalBlue = 97;
export const VeryDarkRoyalBlue = 98;
export const BlueViolet = 18;
export const DarkBlueViolet = 99;
export const VeryDarkBlueViolet = 100;
export const Violet = 19;
export const DarkViolet = 101;
export const VeryDarkViolet = 102;
export const ElectricViolet = 20;
export const DeepElectricViolet = 103;
export const VeryDeepElectricViolet = 104;
export const HotMagenta = 21;
export const DarkHotMagenta = 105;
export const VeryDarkHotMagenta = 106;
export const Purple = 22;
export const DarkPurple = 107;
export const VeryDarkPurple = 108;
export const NeonPink = 23;
export const DarkNeonPink = 109;
export const VeryDarkNeonPink = 110;
export const Rose = 24;
export const DarkRose = 111;
export const VeryDarkRose = 112;
export const BrightPink = 25;
export const DarkBrightPink = 113;
export const VeryDarkBrightPink = 114;
export const LightMagenta = 26;
export const DarkLightMagenta = 115;
export const VeryDarkLightMagenta = 116;

// --- SATURATION VARIANTS (27–35) ---
export const RustRed = 27;
export const BurntOrange = 28;
export const Mustard = 29;
export const YellowGreen = 30;
export const Lime = 31;
export const DeepGreen = 32;
export const Blue = 33;
export const Lilac = 34;
export const Mauve = 35;

// --- PASTELS / LIGHT TONES (36–64) ---
export const PaleSalmon = 36;
export const LightOrange = 37;
export const LightAmber = 38;
export const Sand = 39;
export const LightYellow2 = 40;
export const PaleOlive = 41;
export const PaleLime = 42;
export const PaleGreen = 43;
export const MintGreen = 44;
export const OliveGreen = 45;
export const PaleCyan = 46;
export const SkyBlue = 47;
export const LightBlue = 48;
export const MutedBlue = 49;
export const LavenderBlue = 50;
export const PaleLavender = 51;
export const PalePink = 52;
export const PaleSeaGreen = 53;
export const GreyGreen = 54;
export const GreyBlue = 55;
export const SteelGrey = 56;
export const LavenderGrey = 57;
export const DarkSteel = 58;
export const MauveGrey = 59;
export const WarmGrey = 60;
export const OliveGrey = 61;
export const SageGrey = 62;
export const RoseGrey = 63;
export const BrownGrey = 64;

// --- PRIMARY COLORS ---
export const PureBlue = 125;
export const PureGreen = 126;
export const PureRed = 127;

export const colourNames = {  // for pads, steps and play, rec, and record leds
  0: "Black",
  1: "Bright Red",
  2: "Orange Red",
  3: "Bright Orange",
  4: "Tan / Muted Orange",
  5: "Light Yellow",
  6: "Ochre",
  7: "Vivid Yellow",
  8: "Bright Yellow",
  9: "Bright Lime",
  10: "Dull Green",
  11: "Neon Green",
  12: "Teal Green",
  13: "Muted Teal",
  14: "Cyan-Teal",
  15: "Teal-Cyan",
  16: "Azure Blue",
  17: "Royal Blue",
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
  36: "Pale Salmon",
  37: "Light Orange",
  38: "Light Amber",
  39: "Sand",
  40: "Light Yellow 2",
  41: "Pale Olive",
  42: "Pale Lime",
  43: "Pale Green",
  44: "Mint Green",
  45: "Olive Green",
  46: "Pale Cyan",
  47: "Sky Blue",
  48: "Light Blue",
  49: "Muted Blue",
  50: "Lavender Blue",
  51: "Pale Lavender",
  52: "Pale Pink",
  53: "Pale Sea Green",
  54: "Grey Green",
  55: "Grey Blue",
  56: "Steel Grey",
  57: "Lavender Grey",
  58: "Dark Steel",
  59: "Mauve Grey",
  60: "Warm Grey",
  61: "Olive Grey",
  62: "Sage Grey",
  63: "Rose Grey",
  64: "Brown Grey",
  65: "Deep Red",
  66: "Very Dark Red",
  67: "Deep Orange Red",
  68: "Very Dark Orange Red",
  69: "Burnt Sienna",
  70: "Dark Brown",
  71: "Dark Rust",
  72: "Dark Orange",
  73: "Dark Yellow",
  74: "Very Dark Yellow",
  75: "Dark Brown 2",
  76: "Deep Brown",
  77: "Olive",
  78: "Dark Olive",
  79: "Dark Yellow-Olive",
  80: "Very Dark Yellow-Olive",
  81: "Dark Lime",
  82: "Very Dark Lime",
  83: "Dark Green",
  84: "Very Dark Green",
  85: "Dark Grass",
  86: "Very Dark Grass",
  87: "Dark Teal Green",
  88: "Very Dark Teal Green",
  89: "Dark Muted Teal",
  90: "Very Dark Muted Teal",
  91: "Dark Cyan-Teal",
  92: "Very Dark Cyan-Teal",
  93: "Deep Teal-Cyan",
  94: "Very Deep Teal-Cyan",
  95: "Dark Azure",
  96: "Very Dark Azure",
  97: "Dark Royal Blue",
  98: "Very Dark Royal Blue",
  99: "Dark Blue-Violet",
  100: "Very Dark Blue-Violet",
  101: "Dark Violet",
  102: "Very Dark Violet",
  103: "Deep Electric Violet",
  104: "Very Deep Electric Violet",
  105: "Dark Hot Magenta",
  106: "Very Dark Hot Magenta",
  107: "Dark Purple",
  108: "Very Dark Purple",
  109: "Dark Neon Pink",
  110: "Very Dark Neon Pink",
  111: "Dark Rose",
  112: "Very Dark Rose",
  113: "Dark Bright Pink",
  114: "Very Dark Bright Pink",
  115: "Dark Light Magenta",
  116: "Very Dark Light Magenta",
  117: "Black (dup)",
  118: "Light Grey",
  119: "Dark Grey",
  120: "White",
  121: "Light Grey (dup)",
  122: "White (dup)",
  123: "Dark Grey (dup)",
  124: "Dark Grey 2",
  125: "Blue",
  126: "Green",
  127: "Red",
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
export const MoveSample = 118;   // Alias for MoveRecord (Sample button)
export const MoveDelete = 119;

// Groupings
export const MovePads = Array.from({length: 32}, (x, i) => i + 68);
export const MoveSteps = Array.from({length: 16}, (x, i) => i + 16);
export const MoveCCButtons = [
  MoveMainButton,
  MoveBack,
  MoveMenu,
  MovePlay,
  MoveRec,
  MoveCapture,
  MoveRecord,
  MoveSample,
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
  MoveRecord,
  MoveRow1,
  MoveRow2,
  MoveRow3,
  MoveRow4
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

// White LED Brightness (for Menu, Back, Capture, Shift, arrows, etc.)
// These buttons have white LEDs, not RGB - use brightness values 0-127
export const WhiteLedOff = 0x00;
export const WhiteLedDim = 0x10;      // 16 - subtle
export const WhiteLedMedium = 0x40;   // 64 - medium
export const WhiteLedBright = 0x7c;   // 124 - bright (max visible)

/* ============================================================================
 * Backward-compatible aliases
 * ============================================================================
 * Names that existed before the palette refactor. Every one maps to a
 * surviving constant with the IDENTICAL value, so colours are unchanged —
 * these exist purely so modules keep loading.
 *
 * An ES module import of a missing export is a link-time failure: the module
 * does not degrade, it fails to load outright. Dropping these broke the
 * host's own built-ins (song-mode, controller, text_entry) as well as
 * Performance FX, fourtrack and signalscope. Do not remove without auditing
 * every module repo. */
export const Brick = DeepOrangeRed;  /* 67 */
export const Bright = BrightOrange;  /* 3 */
export const BrightGreen = BrightYellow;  /* 8 */
export const BrownYellow = DarkBrown2;  /* 75 */
export const CoolBlue = DarkRoyalBlue;  /* 97 */
export const Cyan = CyanTeal;  /* 14 */
export const DarkBlue = DarkAzure;  /* 95 */
export const DarkGrassGreen = DarkGrass;  /* 85 */
export const DarkIndigo = VeryDarkViolet;  /* 102 */
export const DarkOliveGreen = DarkGreen;  /* 83 */
export const DarkTeal = DarkTealGreen;  /* 87 */
export const DeepBlue = DeepTealCyan;  /* 93 */
export const DeepBlueIndigo = VeryDarkBlueViolet;  /* 100 */
export const DeepBrownYellow = DeepBrown;  /* 76 */
export const DeepMagenta = DarkNeonPink;  /* 109 */
export const DeepPlum = VeryDarkHotMagenta;  /* 106 */
export const DeepTeal = VeryDarkMutedTeal;  /* 90 */
export const DeepViolet = VeryDeepElectricViolet;  /* 104 */
export const DeepWine = VeryDarkBrightPink;  /* 114 */
export const DullOlive = DarkLime;  /* 81 */
export const DullYellow = DarkYellow;  /* 73 */
export const DuskyMauve = DarkLightMagenta;  /* 115 */
export const DustyRose = DarkRose;  /* 111 */
export const ForestGreen = BrightLime;  /* 9 */
export const Green = PureGreen;  /* 126 */
export const Indigo = DarkBlueViolet;  /* 99 */
export const MutedSeaGreen = DarkMutedTeal;  /* 89 */
export const MutedViolet = DarkHotMagenta;  /* 105 */
export const Navy = RoyalBlue;  /* 17 */
export const PurpleBlue = DarkViolet;  /* 101 */
export const Red = PureRed;  /* 127 */
export const ShadowMauve = VeryDarkLightMagenta;  /* 116 */
export const WinePurple = VeryDarkNeonPink;  /* 110 */
