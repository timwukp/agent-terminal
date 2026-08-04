/* runner.h — minimal test harness, zero dependencies. */
#ifndef AT_TEST_RUNNER_H
#define AT_TEST_RUNNER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_failures = 0;
static int t_checks = 0;
static const char *t_current = "";

#define TEST(name) static void name(void)

#define RUN(name)                                                              \
    do {                                                                       \
        t_current = #name;                                                     \
        name();                                                                \
    } while (0)

#define ASSERT_TRUE(cond)                                                     \
    do {                                                                       \
        t_checks++;                                                            \
        if (!(cond)) {                                                         \
            t_failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,        \
                    t_current, #cond);                                         \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_INT(a, b)                                                    \
    do {                                                                       \
        t_checks++;                                                            \
        long long va = (long long)(a), vb = (long long)(b);                    \
        if (va != vb) {                                                        \
            t_failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d [%s] %s=%lld != %s=%lld\n", __FILE__,  \
                    __LINE__, t_current, #a, va, #b, vb);                      \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_MEM(a, b, n)                                                 \
    do {                                                                       \
        t_checks++;                                                            \
        if (memcmp((a), (b), (n)) != 0) {                                      \
            t_failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d [%s] memcmp(%s, %s, %zu)\n", __FILE__, \
                    __LINE__, t_current, #a, #b, (size_t)(n));                 \
        }                                                                      \
    } while (0)

#define TEST_MAIN_END()                                                        \
    do {                                                                       \
        fprintf(stderr, "%s: %d checks, %d failures\n", __FILE__, t_checks,    \
                t_failures);                                                   \
        return t_failures ? 1 : 0;                                             \
    } while (0)

#endif
