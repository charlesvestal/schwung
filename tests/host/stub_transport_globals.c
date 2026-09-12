/*
 * stub_transport_globals — the two transport globals shadow_led_queue.c reads.
 *
 * They really live in shadow_sampler.c, which no host test wants to link: it
 * pulls in the whole sampler, its file I/O and its host callback struct. The
 * LED scan needs only their VALUES, so a test that exercises the scan can
 * supply them directly.
 *
 * Kept in its own file rather than inside a test, because more than one test
 * links shadow_led_queue.c and duplicating the definitions is how they drift.
 * Never linked alongside shadow_sampler.c -- that would be a duplicate symbol,
 * which is the failure you want here rather than a silent second copy.
 */
int shadow_transport_pulses = 0;
int sampler_transport_playing = 0;
