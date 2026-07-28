/* Isolate the per-iteration cost of the forward double->int32 step in
 * each approach's latency chain, for use under `perf stat -e cycles`.
 * Pick which chain to run via argv[1] ("magic" or "naive") so each
 * run's cycle count reflects exactly one approach. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAIRO_MAGIC_NUMBER_FIXED_16_16 (103079215104.0)
#define N_CHAIN (1 << 28)

__attribute__((noinline)) static double
chain_magic(double acc, long n)
{
    for (long i = 0; i < n; i++) {
        union {
            double d;
            int32_t i[2];
        } u;
        u.d = acc + CAIRO_MAGIC_NUMBER_FIXED_16_16;
        int32_t v = u.i[0];
        acc = (double)(int16_t)(v ^ (int)i);
    }
    return acc;
}

__attribute__((noinline)) static double
chain_naive(double acc, long n)
{
    for (long i = 0; i < n; i++) {
        int32_t v = (int32_t)(acc * 65536.0);
        acc = (double)(int16_t)(v ^ (int)i);
    }
    return acc;
}

int main(int argc, char **argv)
{
    double result;
    if (argc > 1 && strcmp(argv[1], "magic") == 0)
        result = chain_magic(0.5, N_CHAIN);
    else
        result = chain_naive(0.5, N_CHAIN);
    printf("result=%g\n", result);
    return 0;
}
