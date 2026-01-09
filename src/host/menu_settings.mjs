/*
 * Settings screen renderer and input handler.
 */

import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '../shared/menu_layout.mjs';

const SETTINGS_COUNT = 5;
const VELOCITY_CURVES = ['linear', 'soft', 'hard', 'full'];
const CLOCK_MODES = ['off', 'internal', 'external'];

export function getSettings() {
    return {
        velocity_curve: host_get_setting('velocity_curve') || 'linear',
        aftertouch_enabled: host_get_setting('aftertouch_enabled') ?? 1,
        aftertouch_deadzone: host_get_setting('aftertouch_deadzone') ?? 0,
        clock_mode: host_get_setting('clock_mode') || 'internal',
        tempo_bpm: host_get_setting('tempo_bpm') ?? 120
    };
}

function capitalize(s) {
    return s.charAt(0).toUpperCase() + s.slice(1);
}

function formatClockMode(mode) {
    if (mode === "internal") return "INT";
    if (mode === "external") return "EXT";
    return "OFF";
}

export function drawSettings({ settingsIndex }) {
    drawMenuHeader("Settings");

    const settings = getSettings();
    const items = [
        { label: "Velocity", value: capitalize(settings.velocity_curve) },
        { label: "Aftertouch", value: settings.aftertouch_enabled ? "On" : "Off" },
        { label: "AT Deadzone", value: String(settings.aftertouch_deadzone) },
        { label: "MIDI Clock", value: formatClockMode(settings.clock_mode) },
        { label: "Tempo BPM", value: String(settings.tempo_bpm) }
    ];

    drawMenuList({
        items,
        selectedIndex: settingsIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => `${item.label}:`,
        getValue: (item) => item.value
    });

    drawMenuFooter("Back:back  </>:change");
}

export function handleSettingsCC({ cc, value, settingsIndex, shiftHeld }) {
    const isDown = value > 0;
    let nextIndex = settingsIndex;

    if (cc === 14) {
        if (value === 1) {
            nextIndex = Math.min(settingsIndex + 1, SETTINGS_COUNT - 1);
        } else if (value === 127 || value === 65) {
            nextIndex = Math.max(settingsIndex - 1, 0);
        }
        return { nextIndex };
    }

    if (cc === 54 && isDown) {
        nextIndex = Math.min(settingsIndex + 1, SETTINGS_COUNT - 1);
        return { nextIndex };
    }
    if (cc === 55 && isDown) {
        nextIndex = Math.max(settingsIndex - 1, 0);
        return { nextIndex };
    }

    if (cc === 62 && isDown) {
        changeSettingValue(settingsIndex, -1, shiftHeld);
        return { nextIndex };
    }
    if (cc === 63 && isDown) {
        changeSettingValue(settingsIndex, 1, shiftHeld);
        return { nextIndex };
    }

    return { nextIndex };
}

function changeSettingValue(settingsIndex, delta, shiftHeld) {
    const settings = getSettings();

    if (settingsIndex === 0) {
        let idx = VELOCITY_CURVES.indexOf(settings.velocity_curve);
        idx = (idx + delta + VELOCITY_CURVES.length) % VELOCITY_CURVES.length;
        host_set_setting('velocity_curve', VELOCITY_CURVES[idx]);
    } else if (settingsIndex === 1) {
        host_set_setting('aftertouch_enabled', settings.aftertouch_enabled ? 0 : 1);
    } else if (settingsIndex === 2) {
        let dz = settings.aftertouch_deadzone;
        const step = shiftHeld ? 1 : 5;
        dz = dz + (delta * step);
        if (dz < 0) dz = 0;
        if (dz > 50) dz = 50;
        host_set_setting('aftertouch_deadzone', dz);
    } else if (settingsIndex === 3) {
        let idx = CLOCK_MODES.indexOf(settings.clock_mode);
        idx = (idx + delta + CLOCK_MODES.length) % CLOCK_MODES.length;
        host_set_setting('clock_mode', CLOCK_MODES[idx]);
    } else if (settingsIndex === 4) {
        let bpm = settings.tempo_bpm;
        const step = shiftHeld ? 1 : 5;
        bpm = bpm + (delta * step);
        if (bpm < 20) bpm = 20;
        if (bpm > 300) bpm = 300;
        host_set_setting('tempo_bpm', bpm);
    }

    host_save_settings();
}
