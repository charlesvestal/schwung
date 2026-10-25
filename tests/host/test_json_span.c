/*
 * json_span_end must match brackets the way JSON does: a brace or bracket
 * inside a STRING is text, not structure.
 *
 * shadow_save_state() copies the "patches" array and the "master_fx_chain"
 * object out of shadow_config.json by matching brackets. It counted them
 * blindly, so a user-typed name containing } or ] (a Master FX preset called
 * "Pad }") ended the copy early and the file was rewritten as invalid JSON.
 */
#include <stdio.h>
#include <string.h>
#include "json_span.h"

static int fails = 0;
static void check(const char *label, const char *json, const char *want_tail)
{
    const char *start = strpbrk(json, "{[");
    const char *end = start ? json_span_end(start) : NULL;
    if (want_tail == NULL) {
        if (end != NULL) { printf("FAIL %s: expected unterminated, got end\n", label); fails++; }
        return;
    }
    if (!end || strcmp(end, want_tail) != 0) {
        printf("FAIL %s: tail after span = \"%s\", want \"%s\"\n", label, end ? end : "(null)", want_tail);
        fails++;
    }
}

int main(void)
{
    check("plain object", "{\"a\":1},X", ",X");
    check("nested", "{\"a\":{\"b\":[1,{\"c\":2}]}} rest", " rest");
    check("brace in string", "{\"user_preset\":{\"name\":\"Pad }\"}},X", ",X");
    check("open brace in string", "{\"name\":\"{{{\"},X", ",X");
    check("bracket in string (array)", "[{\"name\":\"a ] b\"}],X", ",X");
    check("escaped quote", "{\"name\":\"say \\\"}\\\" ok\"},X", ",X");
    check("escaped backslash before quote", "{\"p\":\"C:\\\\\"},X", ",X");
    check("unterminated", "{\"a\":{\"b\":1}", NULL);
    check("unterminated string", "{\"a\":\"}", NULL);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: json_span_end skips brackets inside strings\n");
    return 0;
}
