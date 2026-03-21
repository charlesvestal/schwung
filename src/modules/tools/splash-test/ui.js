/*
 * Splash Test - Tool module for previewing the splash screen animation
 *
 * Jog click: restart animation
 * Back: exit to tools menu
 */

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* Ball layout — matched to logo-splash.png */
const BALL_RADIUS = 4;
const BALL_Y = 26;              /* resting center Y (all balls in group) */
const NUM_BALLS = 5;

/* Resting X positions: all 5 balls touching with 3px gaps */
const GROUP_X = [45, 56, 67, 78, 89];

/* Raised positions (from image): offset outward with 8px gap, elevated */
const LEFT_RAISED  = { x: 40, y: 17 };
const RIGHT_RAISED = { x: 95, y: 17 };

/* Impact flash lines */
const IMPACT_LINE_LEN = 1;      /* length of each impact line */
const IMPACT_GAP = 2;           /* gap between ball edge and line start */
const IMPACT_FLASH_TICKS = 20;   /* how many ticks the lines are visible */

/* Animation timing (in ticks, ~44/sec) */
const PRE_HOLD_TICKS = 11;      /* ~0.25s ball sits raised before moving */
const TENSION_TICKS = 10;       /* ~0.35s tension build */
const SNAP_TICKS = 0;           /* instant snap */
const TRANSFER_TICKS = 0;       /* instant */
const RELEASE_TICKS = 5;        /* ~0.11s — SHOOT up */
const HOLD_TICKS = 35;          /* ~0.8s hold */
const TOTAL_TICKS = PRE_HOLD_TICKS + TENSION_TICKS + SNAP_TICKS + TRANSFER_TICKS + RELEASE_TICKS + HOLD_TICKS;

/* Logo */
const LOGO_PATH = "/data/UserData/schwung/host/logo-text.png";
const CIRCLE_PATH = "/data/UserData/schwung/host/logo-circle.png";

let tick_count = 0;
let animDone = false;

/* MIDI constants */
const CC_JOG_CLICK = 3;
const CC_BACK = 51;

/* Ease-in: power of 5 — barely moves then WHIPS */
function easeInHard(t) {
    return t * t * t * t * t;
}

/* Ease-out: fast start, decelerates hard */
function easeOutHard(t) {
    return 1 - Math.pow(1 - t, 4);
}

/*
 * Compute ball position along a parametric arc.
 * progress: 0 = raised position, 1 = resting position
 * The arc bulges downward (below the straight line) for a swooping feel.
 */
function arcPos(raised, restX, restY, progress) {
    /* Linear interpolation for X */
    const x = raised.x + (restX - raised.x) * progress;

    /* Quarter-circle curve: smooth arc from raised down to rest */
    const angle = progress * Math.PI / 2;  /* 0 to 90 degrees */
    const y = raised.y + (restY - raised.y) * Math.sin(angle);

    return { x: Math.round(x), y: Math.round(y) };
}

function drawSplash() {
    clear_screen();

    /* progress: 0 = raised, 1 = at rest in group */
    let leftProgress = 0;
    /* rightProgress: 0 = at rest, 1 = raised */
    let rightProgress = 0;
    const t = tick_count;

    const preHoldEnd = PRE_HOLD_TICKS;
    const tensionEnd = preHoldEnd + TENSION_TICKS;
    const snapEnd = tensionEnd + SNAP_TICKS;
    const transferEnd = snapEnd + TRANSFER_TICKS;
    const releaseEnd = transferEnd + RELEASE_TICKS;

    if (t < preHoldEnd) {
        /* Pre-hold: ball sits raised, still */
        leftProgress = 0;
    } else if (t < tensionEnd) {
        /* Left ball: slow ease-in from raised toward rest */
        const p = (t - preHoldEnd) / TENSION_TICKS;
        leftProgress = easeInHard(p);
    } else if (t < snapEnd) {
        /* Final snap to rest */
        const p = (t - tensionEnd) / SNAP_TICKS;
        const startP = easeInHard(1.0);
        leftProgress = startP + (1 - startP) * p;
    } else if (t < transferEnd) {
        /* Impact — both at rest */
        leftProgress = 1;
        rightProgress = 0;
    } else if (t < releaseEnd) {
        /* Right ball SHOOTS up */
        const p = (t - transferEnd) / RELEASE_TICKS;
        leftProgress = 1;
        rightProgress = easeOutHard(p);
    } else {
        /* Hold */
        leftProgress = 1;
        rightProgress = 1;
    }

    /* Draw the 5 balls */
    for (let i = 0; i < NUM_BALLS; i++) {
        let x = GROUP_X[i];
        let y = BALL_Y;

        if (i === 0) {
            /* Left ball — arc from raised to rest */
            const pos = arcPos(LEFT_RAISED, GROUP_X[0], BALL_Y, leftProgress);
            x = pos.x;
            y = pos.y;
        } else if (i === NUM_BALLS - 1) {
            /* Right ball — arc from rest to raised */
            const pos = arcPos(RIGHT_RAISED, GROUP_X[4], BALL_Y, 1 - rightProgress);
            x = pos.x;
            y = pos.y;
        }

        draw_image(CIRCLE_PATH, x - 4, y - 4, 128, 0);
    }

    /* Impact flash lines radiating RIGHT from ball 4's right edge */
    const impactStart = tensionEnd;
    const ticksSinceImpact = t - impactStart;
    if (ticksSinceImpact >= 0 && ticksSinceImpact < IMPACT_FLASH_TICKS) {
        const rx = GROUP_X[3] + BALL_RADIUS + IMPACT_GAP;
        const ry = BALL_Y;
        const d45 = Math.round(IMPACT_LINE_LEN * 0.707);
        /* Horizontal (middle) */
        draw_line(rx, ry, rx + IMPACT_LINE_LEN, ry, 1);
        /* Up-right with 2px vertical spacing */
        draw_line(rx, ry - 5, rx + d45, ry - 5 - d45, 1);
        /* Down-right with 2px vertical spacing */
        draw_line(rx, ry + 5, rx + d45, ry + 5 + d45, 1);
    }

    /* Draw logo text */
    draw_image(LOGO_PATH, 9, 37, 128, 0);

    /* Version centered below logo */
    const ver = "v0.8.3";
    const verW = text_width(ver);
    print(Math.round((SCREEN_WIDTH - verW) / 2), 56, ver, 1);

    /* Show hint when animation is done */
    if (animDone) {
        print(2, SCREEN_HEIGHT - 8, "Click: replay  Back: exit", 1);
    }
}

globalThis.init = function() {
    tick_count = 0;
    animDone = false;
};

globalThis.tick = function() {
    if (!animDone) {
        tick_count++;
        if (tick_count >= TOTAL_TICKS) {
            animDone = true;
        }
    }
    drawSplash();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    if (status !== 0xB0) return;

    const cc = data[1];
    const value = data[2];
    if (value === 0) return;

    if (cc === CC_JOG_CLICK) {
        tick_count = 0;
        animDone = false;
    } else if (cc === CC_BACK) {
        host_exit_module();
    }
};
