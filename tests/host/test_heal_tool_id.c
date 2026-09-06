/*
 * test_heal_tool_id — the directory-name filter schwung-heal applies before it
 * will install a standalone tool's staged helper as root-owned 04755.
 *
 * Worth a unit of its own because the binary is untestable end-to-end here: it
 * calls setuid(0) at startup and bails when it is not root, so the policy is
 * only reachable while it lives in a header. The names it screens come from a
 * directory scan of an ableton-writable tree.
 */
#include <stdio.h>
#include <string.h>

#include "heal_tool_id.h"

static int failures = 0;

static void expect(const char *id, int want, const char *why) {
    int got = heal_tool_id_is_safe(id);
    if (got != want) {
        printf("FAIL: heal_tool_id_is_safe(\"%s\") = %d, want %d (%s)\n",
               id ? id : "(null)", got, want, why);
        failures++;
    }
}

int main(void) {
    /* Ordinary module ids in the catalog's shape. */
    expect("davebox-sa", 1, "hyphen is a normal module id");
    expect("seq-test", 1, "hyphen is a normal module id");
    expect("m8", 1, "short alphanumeric");
    expect("rnbo_runner", 1, "underscore");
    expect("tool.v2", 1, "interior dot");
    expect("ABC123", 1, "uppercase and digits");

    /* Leading dot: excludes "." and ".." along with hidden dirs, which is what
     * keeps a scan from walking up out of the tools directory. */
    expect(".", 0, "self");
    expect("..", 0, "parent");
    expect(".hidden", 0, "hidden dir");

    /* Anything that could be read as more than a name. A slash is the one that
     * matters most: the id is pasted into a path. */
    expect("a/b", 0, "slash");
    expect("../evil", 0, "traversal");
    expect("$(id)", 0, "command substitution");
    expect("a b", 0, "space");
    expect("a\nb", 0, "newline");
    expect("a;b", 0, "semicolon");
    expect("a*", 0, "glob");
    expect("a'b", 0, "quote");

    /* Degenerate input. */
    expect("", 0, "empty");
    expect(NULL, 0, "null");

    if (failures) {
        printf("test_heal_tool_id: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_heal_tool_id: PASS\n");
    return 0;
}
