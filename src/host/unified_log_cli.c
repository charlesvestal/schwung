#include "unified_log.h"

#include <stdio.h>
#include <string.h>

static int parse_level(const char *value)
{
    if (!value) return -1;
    if (strcmp(value, "ERROR") == 0) return LOG_LEVEL_ERROR;
    if (strcmp(value, "WARN") == 0) return LOG_LEVEL_WARN;
    if (strcmp(value, "INFO") == 0) return LOG_LEVEL_INFO;
    if (strcmp(value, "DEBUG") == 0) return LOG_LEVEL_DEBUG;
    return -1;
}

static void trim_newline(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <source> [ERROR|WARN|INFO|DEBUG] [message...]\n", argv[0]);
        return 2;
    }

    const char *source = argv[1];
    int level = LOG_LEVEL_INFO;
    int argi = 2;

    if (argi < argc) {
        int parsed = parse_level(argv[argi]);
        if (parsed >= 0) {
            level = parsed;
            argi++;
        }
    }

    unified_log_init();

    if (argi < argc) {
        char message[2048];
        size_t used = 0;
        message[0] = '\0';

        for (int i = argi; i < argc; i++) {
            int written = snprintf(message + used, sizeof(message) - used,
                                   "%s%s", used ? " " : "", argv[i]);
            if (written < 0 || (size_t)written >= sizeof(message) - used) {
                used = sizeof(message) - 1;
                break;
            }
            used += (size_t)written;
        }

        unified_log(source, level, "%s", message);
        unified_log_shutdown();
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        trim_newline(line);
        if (!line[0]) continue;
        unified_log(source, level, "%s", line);
    }

    unified_log_shutdown();
    return ferror(stdin) ? 1 : 0;
}
