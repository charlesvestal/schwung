#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Forward declaration of the actual function from the file under test */
void *mallocz(size_t size);

START_TEST(test_mallocz_null_pointer_dereference)
{
    /* Invariant: mallocz must not dereference NULL when malloc fails */
    const size_t payloads[] = {
        SIZE_MAX,        /* Exploit case: triggers malloc failure */
        0,               /* Boundary case: zero allocation */
        1024             /* Valid input: normal allocation */
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        /* The test passes if mallocz returns or crashes gracefully;
           a segfault will be caught by the test runner as a failure */
        void *result = mallocz(payloads[i]);
        
        /* If malloc succeeded (result != NULL), we can free it */
        if (result != NULL) {
            free(result);
        }
        /* If malloc failed (result == NULL), the invariant holds 
           because memset was not called with NULL */
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_mallocz_null_pointer_dereference);
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