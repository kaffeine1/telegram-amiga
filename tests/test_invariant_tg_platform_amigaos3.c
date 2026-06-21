#include <check.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration - the actual function from the platform file */
extern unsigned long tg_os3_entropy_gather(unsigned char *buf, unsigned long cap);

START_TEST(test_entropy_gather_minimum_entropy)
{
    /* Invariant: Entropy gathering must produce non-trivial, non-repeating output
     * even under adversarial timing conditions (attacker knows approximate time) */
    
    unsigned long test_sizes[] = {
        0,      /* Boundary: zero-length buffer */
        32,     /* Minimum useful entropy buffer */
        256,    /* Standard entropy request */
    };
    int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        unsigned char buf1[256] = {0};
        unsigned char buf2[256] = {0};
        unsigned long cap = test_sizes[i];
        
        unsigned long n1 = tg_os3_entropy_gather(buf1, cap);
        unsigned long n2 = tg_os3_entropy_gather(buf2, cap);
        
        /* Property 1: Must not write beyond requested capacity */
        ck_assert_uint_le(n1, cap);
        ck_assert_uint_le(n2, cap);
        
        /* Property 2: For non-zero requests, must gather some entropy */
        if (cap > 0) {
            ck_assert_uint_gt(n1, 0);
            ck_assert_uint_gt(n2, 0);
        }
        
        /* Property 3: Consecutive calls should produce different output
         * (critical for DRBG seeding - identical seeds = predictable keys) */
        if (cap >= 32 && n1 >= 32 && n2 >= 32) {
            ck_assert_msg(memcmp(buf1, buf2, 32) != 0,
                "Entropy gather produced identical output on consecutive calls");
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_entropy_gather_minimum_entropy);
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