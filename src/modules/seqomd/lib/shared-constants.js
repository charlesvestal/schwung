/*
 * SEQOMD Local copy of shared constants
 * (Bundled for module independence from host framework)
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

// Internal MIDI Notes
export const MoveKnob1Touch = 0;
export const MoveKnob2Touch = 1;
export const MoveKnob3Touch = 2;
export const MoveKnob4Touch = 3;
export const MoveKnob5Touch = 4;
export const MoveKnob6Touch = 5;
export const MoveKnob7Touch = 6;
export const MoveKnob8Touch = 7;
export const MoveMasterTouch = 8;
export const MoveMainTouch = 9;
export const MoveStep1 = 16;
export const MoveStep2 = 17;
export const MoveStep3 = 18;
export const MoveStep4 = 19;
export const MoveStep5 = 20;
export const MoveStep6 = 21;
export const MoveStep7 = 22;
export const MoveStep8 = 23;
export const MoveStep9 = 24;
export const MoveStep10 = 25;
export const MoveStep11 = 26;
export const MoveStep12 = 27;
export const MoveStep13 = 28;
export const MoveStep14 = 29;
export const MoveStep15 = 30;
export const MoveStep16 = 31;
export const MovePad1 = 68;
export const MovePad32 = 99;

// Internal MIDI CCs
export const MoveMainButton = 3;
export const MoveMainKnob = 14;
export const MoveRow4 = 40;
export const MoveRow3 = 41;
export const MoveRow2 = 42;
export const MoveRow1 = 43;
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
export const MoveKnob1 = 71;
export const MoveKnob2 = 72;
export const MoveKnob3 = 73;
export const MoveKnob4 = 74;
export const MoveKnob5 = 75;
export const MoveKnob6 = 76;
export const MoveKnob7 = 77;
export const MoveKnob8 = 78;
export const MoveMaster = 79;
export const MovePlay = 85;
export const MoveRec = 86;
export const MoveMute = 88;
export const MoveRecord = 118;
export const MoveDelete = 119;

// Step UI LEDs (CC-based, same values as MoveStep notes but for setButtonLED)
export const MoveStep1UI = 16;
export const MoveStep2UI = 17;
export const MoveStep3UI = 18;
export const MoveStep4UI = 19;
export const MoveStep5UI = 20;
export const MoveStep6UI = 21;
export const MoveStep7UI = 22;
export const MoveStep8UI = 23;
export const MoveStep9UI = 24;
export const MoveStep10UI = 25;
export const MoveStep11UI = 26;
export const MoveStep12UI = 27;
export const MoveStep13UI = 28;
export const MoveStep14UI = 29;
export const MoveStep15UI = 30;
export const MoveStep16UI = 31;

// Groupings
export const MovePads = Array.from({length: 32}, (x, i) => i + 68);
export const MoveSteps = Array.from({length: 16}, (x, i) => i + 16);
export const MoveTracks = [MoveRow4, MoveRow3, MoveRow2, MoveRow1];
