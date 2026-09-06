/*
 * heal_tool_id.h — which directory names schwung-heal will treat as a tool id.
 *
 * Split out from schwung-heal.c so tests/host can run the predicate. The
 * binary itself calls setuid(0) at startup and refuses to run unprivileged,
 * so this policy is only testable while it lives apart from the binary.
 *
 * The names come from a directory scan of
 * /data/UserData/schwung/modules/tools — never from argv — and are used to
 * resolve a root-owned 04755 install, so the filter is deliberately narrow:
 * [A-Za-z0-9_.-], and never a leading dot, which excludes "." and ".." along
 * with hidden directories. Everything else is skipped silently.
 *
 * The filter is a first cut, not the safety property: schwung-heal resolves
 * every component of the path with O_NOFOLLOW and operates on descriptors, so
 * a name that gets past this cannot steer a write out of the tool's own
 * directory. See install_one_tool_helper() in src/schwung-heal.c.
 */
#ifndef HEAL_TOOL_ID_H
#define HEAL_TOOL_ID_H

static inline int heal_tool_id_is_safe(const char *id) {
    if (!id || id[0] == '\0' || id[0] == '.') return 0;
    for (const char *c = id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.'))
            return 0;
    }
    return 1;
}

#endif /* HEAL_TOOL_ID_H */
