/* The sampler-source announcement matcher.
 *
 * Every string below marked OBSERVED is copied verbatim out of a device's own
 * debug.log (2026-09-12, 732 D-Bus announcements over 114k log lines). None of
 * them is hypothetical and none is a near-miss constructed to make a point.
 *
 * WHY THE NEGATIVES ARE THE WHOLE TEST. This verdict gates
 * native_resample_bridge_source_allows_apply(): MIC_IN or USB_C_IN stops the
 * bridge writing Schwung's mix into Move's capture buffer. A false positive
 * therefore disables the bridge silently, for the rest of the session, with no
 * trace outside a diagnostic that is off by default — and the verdict LATCHES,
 * so one stray announcement is enough.
 *
 * The same handler (shadow_dbus_handle_text) calls metronome_announce_classify
 * two lines away, and that one was written against exactly this failure after
 * the mute auto-correct had to be removed for it. This matcher was not brought
 * along.
 */
#include <stdio.h>
#include <string.h>
#include "sampler_source_announce.h"

static int failures = 0;

static const char *name(native_sampler_source_t s)
{
    switch (s) {
    case NATIVE_SAMPLER_SOURCE_UNKNOWN:    return "unknown";
    case NATIVE_SAMPLER_SOURCE_RESAMPLING: return "resampling";
    case NATIVE_SAMPLER_SOURCE_LINE_IN:    return "line-in";
    case NATIVE_SAMPLER_SOURCE_MIC_IN:     return "mic-in";
    case NATIVE_SAMPLER_SOURCE_USB_C_IN:   return "usb-c-in";
    }
    return "?";
}

static void expect(const char *in, native_sampler_source_t want, const char *what)
{
    native_sampler_source_t got = sampler_source_announce_classify(in);
    if (got != want) {
        printf("FAIL: %s\n      classify(\"%s\")\n      = %s, want %s\n",
               what, in ? in : "(null)", name(got), name(want));
        failures++;
    }
}

int main(void)
{
    /* ---- OBSERVED: Move's USB-C *OUTPUT* menu. -----------------------------
     *
     * This is the confirmed field failure. The USB-C audio-OUT source has
     * nothing to do with what the sampler records, and one of these fired on
     * the device that produced the log:
     *
     *   05:30:45 [shim] Native sampler source: usb-c-in
     *                   (from "USB-C Audio. Submenu. 9 of 12")
     *
     * Scrolling onto a Settings row disabled the resample bridge until reboot.
     */
    expect("USB-C Audio. Submenu. 9 of 12",
           NATIVE_SAMPLER_SOURCE_UNKNOWN, "USB-C output submenu row");
    expect("USB-C Audio Out: Mic. Menu item. 1 of 2",
           NATIVE_SAMPLER_SOURCE_UNKNOWN, "USB-C output menu item, Mic");
    expect("USB-C Audio Out: Main Out. Menu item. 2 of 2",
           NATIVE_SAMPLER_SOURCE_UNKNOWN, "USB-C output menu item, Main Out");

    /* ---- Schwung's own TTS, which returns through the same handler. --------
     *
     * The loopback is not a theory: "Analytics, Off, 1 of 5" and
     * "S4: bouba-kiki Bulge: 27%" are in the same capture, and Schwung is the
     * only thing that utters them. The feedback-gate line is the one that
     * carries "mic".
     */
    expect("Speaker feedback risk. Speakers and mic active. "
           "Plug in headphones. Jog click for yes, back for no.",
           NATIVE_SAMPLER_SOURCE_UNKNOWN, "our own feedback-gate announcement");
    expect("Replaces Mic and Line-in with ME + Move Audio",
           NATIVE_SAMPLER_SOURCE_UNKNOWN, "our own Schwung Mix description");

    /* ---- "mic" is a substring of ordinary words. --------------------------- */
    expect("Dynamic Tube",  NATIVE_SAMPLER_SOURCE_UNKNOWN, "dyna-MIC-tube");
    expect("Dynamics, 3 of 8", NATIVE_SAMPLER_SOURCE_UNKNOWN, "dyna-MIC-s");
    expect("Ceramic",      NATIVE_SAMPLER_SOURCE_UNKNOWN, "cera-MIC");
    expect("Atomic Kit",   NATIVE_SAMPLER_SOURCE_UNKNOWN, "ato-MIC");
    expect("Microtonal",   NATIVE_SAMPLER_SOURCE_UNKNOWN, "MICro-tonal");
    expect("Baseline Input", NATIVE_SAMPLER_SOURCE_UNKNOWN, "base-LINE-IN-put");

    /* ---- OBSERVED: already inert, and must stay so. ----------------------- */
    expect("Sample",                    NATIVE_SAMPLER_SOURCE_UNKNOWN, "Move's Sample button");
    expect("Monitor. Submenu. 8 of 12", NATIVE_SAMPLER_SOURCE_UNKNOWN, "Monitor submenu row");
    expect("Analytics, Off, 1 of 5",    NATIVE_SAMPLER_SOURCE_UNKNOWN, "our own grid cell");
    expect("S4: bouba-kiki Bulge: 27%", NATIVE_SAMPLER_SOURCE_UNKNOWN, "our own param readout");
    expect("",                          NATIVE_SAMPLER_SOURCE_UNKNOWN, "empty");
    expect(NULL,                        NATIVE_SAMPLER_SOURCE_UNKNOWN, "null");

    /* ---- THE TRUE POSITIVES ARE MISSING, AND THAT IS THE FINDING. ---------
     *
     * There is no assertion here that a real sampling-source change is
     * classified correctly, because no such announcement has ever been
     * captured. In 732 announcements from a device in daily use, the only
     * strings this matcher classified as anything other than UNKNOWN were the
     * two USB-C output rows above — and a deliberate change of Move's sampling
     * source, made while the log was running, produced no announcement at all.
     *
     * So the guard has not been observed to receive a single true positive,
     * and adding an invented one here would assert that it works. Filling this
     * section in requires a capture of Move actually announcing its sampling
     * source; until then the honest test is that nothing false gets through.
     */

    if (failures == 0) {
        printf("PASS: sampler source announcements (%s)\n",
               "no false positive reaches the resample-bridge gate");
        return 0;
    }
    printf("\n%d failure(s)\n", failures);
    return 1;
}
