/*
 * test.h - the whole test framework.
 *
 * A real framework would be more than this file needs to be. Tests register
 * themselves, the runner runs them, failures print the file and line and the
 * numbers involved.
 */

#ifndef CS_TEST_H
#define CS_TEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

/* M_PI is not standard C, and MSVC only defines it if you ask before including
 * math.h. The tests use it freely, so define it here once. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef void (*cs_test_fn)(void);

void cs_test_register(const char *name, cs_test_fn fn);
void cs_test_fail(const char *file, int line, const char *fmt, ...);
int  cs_test_run_all(const char *filter);

/* Each TEST(x) defines the body and a constructor-ish registrar that the
 * suite file calls. Doing it without compiler specific attributes keeps this
 * portable, at the cost of one REGISTER line per test in test_main.c. */
#define TEST(name)                                      \
    void cs_test_##name(void);                          \
    void cs_test_##name(void)

#define REGISTER(name) do {                             \
        void cs_test_##name(void);                      \
        cs_test_register(#name, cs_test_##name);        \
    } while (0)

#define CHECK(cond)                                                     \
    do { if (!(cond)) cs_test_fail(__FILE__, __LINE__,                  \
        "expected: %s", #cond); } while (0)

#define CHECK_MSG(cond, ...)                                            \
    do { if (!(cond)) cs_test_fail(__FILE__, __LINE__, __VA_ARGS__); } while (0)

/* Absolute tolerance. */
#define CHECK_NEAR(got, want, tol)                                      \
    do {                                                                \
        const double _g = (double)(got), _w = (double)(want);           \
        if (!(fabs(_g - _w) <= (double)(tol)))                          \
            cs_test_fail(__FILE__, __LINE__,                            \
                "%s = %.6g, expected %.6g +- %.3g (off by %.3g)",       \
                #got, _g, _w, (double)(tol), fabs(_g - _w));            \
    } while (0)

/* Relative tolerance, as a fraction. */
#define CHECK_REL(got, want, frac)                                      \
    do {                                                                \
        const double _g = (double)(got), _w = (double)(want);           \
        const double _t = fabs(_w) * (double)(frac);                    \
        if (!(fabs(_g - _w) <= _t))                                     \
            cs_test_fail(__FILE__, __LINE__,                            \
                "%s = %.6g, expected %.6g +- %.1f%% (off by %.3g)",      \
                #got, _g, _w, (double)(frac) * 100.0, fabs(_g - _w));   \
    } while (0)

#define CHECK_STR(got, want)                                            \
    do { if (strcmp((got), (want)) != 0)                                \
        cs_test_fail(__FILE__, __LINE__, "%s = \"%s\", expected \"%s\"", \
            #got, (got), (want)); } while (0)

#endif /* CS_TEST_H */
