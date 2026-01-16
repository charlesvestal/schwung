export function processMidi(msg) {
    const status = msg[0] & 0xF0;
    if ((status === 0x90 || status === 0x80) && msg.length >= 3) {
        const note = msg[1] + 12;
        if (note > 127) return [];
        return [[msg[0], note, msg[2]]];
    }

    return [msg];
}
