/* Unit test for the co-run ownership contract (corun_event_owner) — both the
 * new cede-default model and the frozen legacy keep-list model.
 *
 * The cede model is what modules see going forward: a tool KEEPS the whole
 * control surface and lists only what it CEDES to the peer; every routable input
 * is a first-class group. The legacy model is the pre-cede keep-list contract,
 * preserved byte-identically so no existing module's behavior changes.
 *
 * Build/run: bash tests/host/test_corun_cede_default.sh
 */
#include <stdio.h>
#include <string.h>
#include "shadow_constants.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } } while (0)

#define CC   0xB0
#define NOTE 0x90

/* Legacy keep-list module: flags=0, raw keep_mask. */
static shadow_control_t mk_legacy(uint32_t keep_mask) {
    shadow_control_t c;
    memset(&c, 0, sizeof c);
    c.corun.target = CORUN_TARGET_MOVE_NATIVE;
    c.corun.id = 0;
    c.corun.flags = 0;
    c.corun.keep_mask = keep_mask;
    return c;
}

/* New cede-model module: flags=CEDE_MODEL, keep stored as the complement of the
 * cede set (exactly what the host API does at the JS boundary). */
static shadow_control_t mk_cede(uint32_t cede_mask, uint8_t extra_flags) {
    shadow_control_t c;
    memset(&c, 0, sizeof c);
    c.corun.target = CORUN_TARGET_MOVE_NATIVE;
    c.corun.id = 0;
    c.corun.flags = (uint8_t)(CORUN_F_CEDE_MODEL | extra_flags);
    c.corun.keep_mask = corun_cede_to_keep(cede_mask);
    return c;
}

#define OWN(c, type, d1) corun_event_owner(&(c), (type), (d1))

int main(void) {
    /* ---- 1. Full classifier: every routable input maps to a group ---- */
    CHECK(corun_group_for_event(CC, 3)   == CORUN_GRP_JOG,           "jog click -> JOG");
    CHECK(corun_group_for_event(CC, 14)  == CORUN_GRP_JOG,           "jog turn -> JOG");
    CHECK(corun_group_for_event(CC, 40)  == CORUN_GRP_TRACK_BUTTONS, "row -> TRACK_BUTTONS");
    CHECK(corun_group_for_event(CC, 49)  == CORUN_GRP_SHIFT,         "49 -> SHIFT");
    CHECK(corun_group_for_event(CC, 50)  == CORUN_GRP_MENU,          "50 -> MENU");
    CHECK(corun_group_for_event(CC, 51)  == CORUN_GRP_BACK,          "51 -> BACK");
    CHECK(corun_group_for_event(CC, 52)  == CORUN_GRP_CAPTURE,       "52 -> CAPTURE");
    CHECK(corun_group_for_event(CC, 54)  == CORUN_GRP_NAV_DOWN,      "54 -> NAV_DOWN");
    CHECK(corun_group_for_event(CC, 55)  == CORUN_GRP_NAV_UP,        "55 -> NAV_UP");
    CHECK(corun_group_for_event(CC, 56)  == CORUN_GRP_UNDO,          "56 -> UNDO");
    CHECK(corun_group_for_event(CC, 58)  == CORUN_GRP_LOOP,          "58 -> LOOP");
    CHECK(corun_group_for_event(CC, 60)  == CORUN_GRP_COPY,          "60 -> COPY");
    CHECK(corun_group_for_event(CC, 62)  == CORUN_GRP_NAV_LEFT,      "62 -> NAV_LEFT");
    CHECK(corun_group_for_event(CC, 63)  == CORUN_GRP_NAV_RIGHT,     "63 -> NAV_RIGHT");
    CHECK(corun_group_for_event(CC, 71)  == CORUN_GRP_KNOBS,         "71 -> KNOBS");
    CHECK(corun_group_for_event(CC, 78)  == CORUN_GRP_KNOBS,         "78 -> KNOBS");
    CHECK(corun_group_for_event(CC, 79)  == CORUN_GRP_MASTER,        "79 -> MASTER");
    CHECK(corun_group_for_event(CC, 85)  == CORUN_GRP_PLAY,          "85 -> PLAY");
    CHECK(corun_group_for_event(CC, 86)  == CORUN_GRP_REC,           "86 -> REC");
    CHECK(corun_group_for_event(CC, 88)  == CORUN_GRP_MUTE,          "88 -> MUTE");
    CHECK(corun_group_for_event(CC, 118) == CORUN_GRP_SAMPLE,        "118 -> SAMPLE");
    CHECK(corun_group_for_event(CC, 119) == CORUN_GRP_DELETE,        "119 -> DELETE");
    CHECK(corun_group_for_event(NOTE, 0) == CORUN_GRP_TOUCH,         "note0 -> TOUCH (knob)");
    CHECK(corun_group_for_event(NOTE, 9) == CORUN_GRP_TOUCH,         "note9 -> TOUCH (main)");
    CHECK(corun_group_for_event(NOTE, 16)== CORUN_GRP_STEPS,         "note16 -> STEPS");
    CHECK(corun_group_for_event(NOTE, 68)== CORUN_GRP_PADS,          "note68 -> PADS");
    /* Sensors stay unclassified (not routable) */
    CHECK(corun_group_for_event(CC, 114) == 0,                       "114 plug-detect unclassified");
    CHECK(corun_group_for_event(CC, 115) == 0,                       "115 plug-detect unclassified");

    /* ---- 2. Inactive co-run: everything is the tool's ---- */
    { shadow_control_t c = mk_legacy(0); c.corun.target = CORUN_TARGET_NONE;
      CHECK(OWN(c, CC, 14) == CORUN_OWNER_TOOL, "inactive -> TOOL"); }

    /* ---- 3. Legacy model (frozen behavior) ---- */
    /* 3a. keep_mask==0 => default split: keeps pads/steps/transport/menu/mute, cedes jog/knobs */
    { shadow_control_t c = mk_legacy(0);
      CHECK(OWN(c, NOTE, 68) == CORUN_OWNER_TOOL, "legacy default: PADS tool");
      CHECK(OWN(c, CC, 50)   == CORUN_OWNER_TOOL, "legacy default: MENU tool");
      CHECK(OWN(c, CC, 88)   == CORUN_OWNER_TOOL, "legacy default: MUTE tool");
      CHECK(OWN(c, CC, 14)   == CORUN_OWNER_PEER, "legacy default: JOG peer");
      CHECK(OWN(c, CC, 71)   == CORUN_OWNER_PEER, "legacy default: KNOBS peer"); }
    /* 3b. legacy explicit keep cedes the unlisted */
    { shadow_control_t c = mk_legacy(CORUN_GRP_PADS | CORUN_GRP_STEPS);
      CHECK(OWN(c, NOTE, 68) == CORUN_OWNER_TOOL, "legacy keep PADS: tool");
      CHECK(OWN(c, CC, 50)   == CORUN_OWNER_PEER, "legacy keep PADS: MENU ceded"); }
    /* 3c. CRITICAL legacy carve-out: newly-classified buttons stay with a legacy tool */
    { shadow_control_t c = mk_legacy(CORUN_GRP_PADS);
      CHECK(OWN(c, CC, 85)  == CORUN_OWNER_TOOL, "legacy: PLAY stays tool (carve-out)");
      CHECK(OWN(c, CC, 60)  == CORUN_OWNER_TOOL, "legacy: COPY stays tool (carve-out)");
      CHECK(OWN(c, CC, 55)  == CORUN_OWNER_TOOL, "legacy: NAV stays tool (carve-out)"); }
    /* 3d. Back: framework exit unless legacy KEEP_BACK set */
    { shadow_control_t c = mk_legacy(0);
      CHECK(OWN(c, CC, 51)  == CORUN_OWNER_NONE, "legacy: Back -> framework (NONE)"); }
    { shadow_control_t c = mk_legacy(CORUN_KEEP_BACK);
      CHECK(OWN(c, CC, 51)  == CORUN_OWNER_PEER, "legacy KEEP_BACK + BACK not kept -> peer"); }
    { shadow_control_t c = mk_legacy(CORUN_KEEP_BACK | CORUN_GRP_BACK);
      CHECK(OWN(c, CC, 51)  == CORUN_OWNER_TOOL, "legacy KEEP_BACK + BACK kept -> tool"); }

    /* ---- 4. Cede model (the new contract) ---- */
    /* 4a. cede jog+knobs: those go to peer, everything else (incl. new buttons) kept */
    { shadow_control_t c = mk_cede(CORUN_GRP_JOG | CORUN_GRP_KNOBS, 0);
      CHECK(OWN(c, CC, 14)  == CORUN_OWNER_PEER, "cede {JOG,KNOBS}: JOG peer");
      CHECK(OWN(c, CC, 71)  == CORUN_OWNER_PEER, "cede {JOG,KNOBS}: KNOBS peer");
      CHECK(OWN(c, NOTE,68) == CORUN_OWNER_TOOL, "cede {JOG,KNOBS}: PADS kept");
      CHECK(OWN(c, CC, 50)  == CORUN_OWNER_TOOL, "cede {JOG,KNOBS}: MENU kept");
      CHECK(OWN(c, CC, 85)  == CORUN_OWNER_TOOL, "cede {JOG,KNOBS}: PLAY kept by default");
      CHECK(OWN(c, CC, 60)  == CORUN_OWNER_TOOL, "cede {JOG,KNOBS}: COPY kept by default");
      CHECK(OWN(c, CC, 88)  == CORUN_OWNER_TOOL, "cede {JOG,KNOBS}: MUTE kept"); }
    /* 4b. per-button independence: cede COPY only, DELETE/UNDO/nav unaffected */
    { shadow_control_t c = mk_cede(CORUN_GRP_COPY, 0);
      CHECK(OWN(c, CC, 60)  == CORUN_OWNER_PEER, "cede {COPY}: COPY peer");
      CHECK(OWN(c, CC, 119) == CORUN_OWNER_TOOL, "cede {COPY}: DELETE kept");
      CHECK(OWN(c, CC, 56)  == CORUN_OWNER_TOOL, "cede {COPY}: UNDO kept");
      CHECK(OWN(c, CC, 55)  == CORUN_OWNER_TOOL, "cede {COPY}: NAV kept"); }
    /* 4c. cede nothing => tool owns the whole surface */
    { shadow_control_t c = mk_cede(0, 0);
      CHECK(OWN(c, CC, 14)  == CORUN_OWNER_TOOL, "cede {}: JOG kept");
      CHECK(OWN(c, CC, 71)  == CORUN_OWNER_TOOL, "cede {}: KNOBS kept"); }
    /* 4d. Back in cede model: framework exit unless OWN_BACK; then governed by cede */
    { shadow_control_t c = mk_cede(0, 0);
      CHECK(OWN(c, CC, 51)  == CORUN_OWNER_NONE, "cede: Back -> framework (NONE)"); }
    { shadow_control_t c = mk_cede(0, CORUN_F_OWN_BACK);
      CHECK(OWN(c, CC, 51)  == CORUN_OWNER_TOOL, "cede OWN_BACK + not ceded -> tool"); }
    { shadow_control_t c = mk_cede(CORUN_GRP_BACK, CORUN_F_OWN_BACK);
      CHECK(OWN(c, CC, 51)  == CORUN_OWNER_PEER, "cede OWN_BACK + ceded BACK -> peer"); }

    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("PASS: corun cede-default contract (%d checks)\n", 0);
    return 0;
}
