// RNBO Runner — Overtake module with suspend/resume

const RNBO_DIR = '/data/UserData/rnbo';
const CONTROL_BIN = RNBO_DIR + '/bin/rnbomovecontrol';
const SHADOW_CONFIG = RNBO_DIR + '/config/control-startup-shadow.json';
const DISPLAY_CTL = RNBO_DIR + '/scripts/display_ctl';
const HOOKS_DIR = '/data/UserData/schwung/hooks';

let phase = 0;
let displayEnabled = false;
let resuming = false;

/* Check if JACK is already running (suspended session).
 * We use a flag file under /data/UserData since host_file_exists
 * restricts paths to /data/UserData (security validation).
 * Also verify JACK process is actually running (flag survives reboot). */
const JACK_RUNNING_FLAG = '/data/UserData/schwung/jack_running';

function isJackRunning() {
    if (typeof host_file_exists !== "function") return false;
    if (!host_file_exists(JACK_RUNNING_FLAG)) return false;
    /* Flag exists — verify JACK is actually running (flag may be stale from reboot) */
    if (typeof host_system_cmd === "function") {
        /* pgrep returns 0 if found. Write result to a temp check file. */
        host_system_cmd('sh -c "pgrep -x jackd > /dev/null 2>&1 && echo 1 > /data/UserData/schwung/jack_check || echo 0 > /data/UserData/schwung/jack_check"');
        if (typeof host_read_file === "function") {
            var result = host_read_file('/data/UserData/schwung/jack_check');
            if (result && result.trim() === '1') return true;
        }
    }
    /* Flag is stale — clean it up */
    host_system_cmd('rm -f ' + JACK_RUNNING_FLAG);
    return false;
}

globalThis.init = function() {
    phase = 0;
    displayEnabled = false;
    resuming = isJackRunning();
    if (typeof console !== 'undefined') {
        console.log('RNBO Runner init: resuming=' + resuming +
                     ' host_file_exists=' + (typeof host_file_exists) +
                     ' shm=' + (typeof host_file_exists === 'function' ? host_file_exists('/dev/shm/schwung_jack') : 'N/A'));
    }
};

globalThis.tick = function() {
    phase++;

    if (resuming) {
        /* Resume path — JACK is already running */

        // Frame 2: install exit hook (in case it was removed)
        if (phase === 2) {
            host_system_cmd('mkdir -p ' + HOOKS_DIR);
            host_system_cmd('cp /data/UserData/schwung/modules/overtake/rnbo-runner/exit-hook.sh ' + HOOKS_DIR + '/overtake-exit.sh');
            host_system_cmd('chmod +x ' + HOOKS_DIR + '/overtake-exit.sh');
        }

        // Frame 3: show resuming message
        if (phase === 3) {
            clear_screen();
            print(0, 10, 'RNBO Runner', 2);
            print(0, 35, 'Resuming...', 1);
        }

        // Frame 35: restore LEDs after overtake init LED clearing completes (~frame 30)
        if (phase === 35) {
            /* Restore cached JACK LEDs via shim */
            if (typeof console !== 'undefined') {
                console.log('RNBO Runner frame 35: shadow_set_param=' + (typeof shadow_set_param));
            }
            if (typeof shadow_set_param === "function") {
                shadow_set_param(0, "jack:restore_leds", "1");
                if (typeof console !== 'undefined') {
                    console.log('RNBO Runner: called jack:restore_leds');
                }
            }
        }

        // Frame 40: re-enable RNBO display
        if (phase === 40) {
            host_system_cmd('sh -c "' + DISPLAY_CTL + ' 1"');
        }

        // Frame 50: done resuming
        if (phase === 50) {
            displayEnabled = true;
            resuming = false;
        }

        if (displayEnabled) {
            clear_screen();
        }
        return;
    }

    /* Fresh launch path */

    // Frame 2: install exit cleanup hook
    if (phase === 2) {
        host_system_cmd('mkdir -p ' + HOOKS_DIR);
        host_system_cmd('cp /data/UserData/schwung/modules/overtake/rnbo-runner/exit-hook.sh ' + HOOKS_DIR + '/overtake-exit.sh');
        host_system_cmd('chmod +x ' + HOOKS_DIR + '/overtake-exit.sh');
    }

    // Frame 3: launch rnbomovecontrol + create running flag
    if (phase === 3) {
        clear_screen();
        print(0, 10, 'RNBO Runner', 2);
        print(0, 35, 'Loading...', 1);
        host_system_cmd('sh -c "export HOME=/data/UserData LD_LIBRARY_PATH=' + RNBO_DIR + '/lib:$LD_LIBRARY_PATH && nohup ' + CONTROL_BIN + ' -s ' + SHADOW_CONFIG + ' > /data/UserData/schwung/rnbo-runner.log 2>&1 &"');
        if (typeof host_write_file === "function") {
            host_write_file(JACK_RUNNING_FLAG, '1');
        }
    }

    // Frame 50: pin JACK/RNBO to cores 2-3, Move stays on 0-1.
    // Prevents RT scheduling contention between Move firmware and JACK graph.
    if (phase === 50) {
        host_system_cmd('sh -c "for p in $(pgrep -f jackd) $(pgrep rnbomovecontrol) $(pgrep rnbooscquery); do taskset -p 0xc $p 2>/dev/null; done &"');
        host_system_cmd('sh -c "taskset -p 0x3 $(pgrep -f MoveOrigi) 2>/dev/null &"');
    }

    // Frame 100: enable RNBO display
    if (phase === 100 && !displayEnabled) {
        clear_screen();
        host_system_cmd('sh -c "' + DISPLAY_CTL + ' 1"');
        displayEnabled = true;
    }

    // Connect patcher MIDI inputs to system:midi_capture_ext.
    // Polls every ~5s but only runs jack_connect when ports change (graph reload).
    if (phase > 440 && phase % 220 === 0) {
        host_system_cmd('sh -c "' + RNBO_DIR + '/scripts/midi_connect.sh &"');
    }

    // Keep clearing so overtake display doesn't override RNBO
    if (displayEnabled) {
        clear_screen();
    }
};

globalThis.onMidiMessageInternal = function(data) {};
globalThis.onMidiMessageExternal = function(data) {};
