#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api_v1.h"

extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

static int failures;

static void expect(plugin_api_v2_t *api, void *instance,
                   const char *key, const char *expected) {
    char value[128];
    int result = api->get_param(instance, key, value, sizeof(value));
    if (result < 0 || strcmp(value, expected) != 0) {
        fprintf(stderr, "FAIL: %s was '%s', wanted '%s'\n",
                key, result < 0 ? "<error>" : value, expected);
        failures++;
    }
}

int main(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    char metadata[4096];
    if (api->get_param(NULL, "chain_params", metadata, sizeof(metadata)) < 0
            || !strstr(metadata, "input_trim")) {
        fprintf(stderr, "FAIL: instance-free parameter metadata was lost\n");
        return 1;
    }
    void *source = api->create_instance(".", NULL);
    if (!source) return 1;

    api->set_param(source, "input_type", "Guitar");
    api->set_param(source, "input_mode", "Mono (R)");
    api->set_param(source, "input_trim", "23.5");
    api->set_param(source, "output_trim", "-7.5");
    api->set_param(source, "gate_mode", "Manual");
    api->set_param(source, "gate_amount", "71");
    api->set_param(source, "gate_threshold", "-31");
    api->set_param(source, "gate_attack", "7.5");
    api->set_param(source, "gate_hold", "145");
    api->set_param(source, "gate_release", "640");
    api->set_param(source, "gate_range", "27");
    api->set_param(source, "hpf_freq", "80 Hz");
    api->set_param(source, "safety_limiter", "On");
    api->set_param(source, "cable_comp", "High");
    api->set_param(source, "soft_clip", "On");
    api->set_param(source, "riaa_eq", "Off");
    api->set_param(source, "subsonic_freq", "30 Hz");
    api->set_param(source, "hum_notch", "Off");
    api->set_param(source, "hum_freq", "50 Hz");
    api->set_param(source, "hum_filter", "60 Hz");

    char state[2048];
    if (api->get_param(source, "state", state, sizeof(state)) < 0) return 1;
    char too_small[8];
    if (api->get_param(source, "state", too_small, sizeof(too_small)) >= 0) {
        fprintf(stderr, "FAIL: truncated state was reported as valid\n");
        return 1;
    }

    void *restored = api->create_instance(".", NULL);
    if (!restored) return 1;
    api->set_param(restored, "state", state);

    expect(api, restored, "input_type", "Guitar");
    expect(api, restored, "input_mode", "Mono (R)");
    expect(api, restored, "input_trim", "23.5");
    expect(api, restored, "output_trim", "-7.5");
    expect(api, restored, "gate_mode", "Manual");
    expect(api, restored, "gate_amount", "71");
    expect(api, restored, "gate_threshold", "-31");
    expect(api, restored, "gate_attack", "7.5");
    expect(api, restored, "gate_hold", "145");
    expect(api, restored, "gate_release", "640");
    expect(api, restored, "gate_range", "27");
    expect(api, restored, "hpf_freq", "80 Hz");
    expect(api, restored, "safety_limiter", "On");
    expect(api, restored, "cable_comp", "High");
    expect(api, restored, "soft_clip", "On");
    expect(api, restored, "riaa_eq", "Off");
    expect(api, restored, "subsonic_freq", "30 Hz");
    expect(api, restored, "hum_notch", "Off");
    expect(api, restored, "hum_freq", "50 Hz");
    expect(api, restored, "hum_filter", "60 Hz");

    api->destroy_instance(restored);
    api->destroy_instance(source);
    if (failures) return 1;
    puts("PASS: Line In state round-trips every chain parameter");
    return 0;
}
