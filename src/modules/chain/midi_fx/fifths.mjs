export function processMidi(msg) {
    const status = msg[0] & 0xF0;
    if ((status === 0x90 || status === 0x80) && msg.length >= 3) {
        const fifth = msg[1] + 7;
        if (fifth > 127) return [msg];
        return [
            [msg[0], msg[1], msg[2]],
            [msg[0], fifth, msg[2]]
        ];
    }

    return [msg];
}
