#include "minunit.h"

char *test_sample() {

    if (0) return "Example return";

    return 0;
}



char *all_tests() {
    mu_suite_start();

    mu_run_test(test_sample);

    return 0;
}

RUN_TEST(all_tests)
