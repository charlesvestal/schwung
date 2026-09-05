/*
 * json_object_compact_copy — the loader-side minifier that hands modules
 * their state in the encoding they emitted (compact), never the raw
 * pretty-printed slot-file slice.
 *
 * The case that matters most is the field one: a JSON.stringify(w, null, 2)
 * rendering of a state object must compact back to byte-equality with the
 * compact original, because module parsers match patterns like `"patch":"`
 * against it. Whitespace INSIDE strings is content and must survive; the
 * escape rules mean a `\"` does not end a string and a `}` inside one does
 * not close the object.
 */
#include <stdio.h>
#include <string.h>

#include "json_compact.h"

static int failures;

static void expect(const char *label, const char *src, const char *want)
{
    char out[512] = "unchanged";
    int got = json_object_compact_copy(out, sizeof(out), src);
    if (got != (int)strlen(want) || strcmp(out, want) != 0) {
        fprintf(stderr, "FAIL %s: got %d [%s], want %zu [%s]\n",
                label, got, out, strlen(want), want);
        failures++;
    }
}

static void expect_rejected(const char *label, const char *src, size_t cap)
{
    char out[512];
    if (cap > sizeof(out)) cap = sizeof(out);
    if (json_object_compact_copy(out, cap, src) >= 0) {
        fprintf(stderr, "FAIL %s: was accepted\n", label);
        failures++;
    }
}

int main(void)
{
    /* The exact defect shape: the pretty form of a state whose parser
     * matches `"patch":"`. Compaction must restore byte-equality. */
    expect("pretty state compacts to the emitted form",
           "{\n"
           "  \"version\": 2,\n"
           "  \"preset\": 37,\n"
           "  \"patch\": \"00AB63\",\n"
           "  \"nested\": {\n"
           "    \"x\": 1\n"
           "  },\n"
           "  \"arr\": [ 1, 2, 3 ]\n"
           "}",
           "{\"version\":2,\"preset\":37,\"patch\":\"00AB63\","
           "\"nested\":{\"x\":1},\"arr\":[1,2,3]}");

    /* Compact input is already the answer. */
    expect("compact input unchanged",
           "{\"a\":1,\"b\":\"two\"}", "{\"a\":1,\"b\":\"two\"}");

    /* String contents are content: inner whitespace, braces, colons and
     * escaped quotes all survive byte-for-byte, and none of them terminate
     * the scan early. */
    expect("string contents preserved",
           "{ \"name\" : \"a } b { c \\\" d : e  f\" }",
           "{\"name\":\"a } b { c \\\" d : e  f\"}");
    expect("escaped backslash before quote still ends the string",
           "{ \"k\" : \"x\\\\\" , \"n\" : 1 }",
           "{\"k\":\"x\\\\\",\"n\":1}");

    /* Only the addressed object is copied — a sibling after its closing
     * brace is not. This is what lets the callers point it at a slice of a
     * larger file. */
    expect("stops at the matching brace",
           "{ \"a\": { \"b\": 1 } } , \"next\": 2",
           "{\"a\":{\"b\":1}}");

    expect_rejected("not an object", "\"str\"", 512);
    expect_rejected("unterminated object", "{ \"a\": { \"b\": 1 }", 512);
    expect_rejected("unterminated string", "{ \"a\": \"oops }", 512);
    expect_rejected("output too small for compact form", "{\"abcdef\":1}", 8);

    if (failures) return 1;
    puts("PASS: json_object_compact_copy minifies outside strings only");
    return 0;
}
