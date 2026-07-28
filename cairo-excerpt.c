/* This is selected code copied from Cairo's sources. */

/* This is the "magic number" approach to converting a double into fixed
 * point as described here:
 *
 * http://www.stereopsis.com/sree/fpu2006.html (an overview)
 * http://www.d6.com/users/checker/pdfs/gdmfp.pdf (in detail)
 *
 * The basic idea is to add a large enough number to the double that the
 * literal floating point is moved up to the extent that it forces the
 * double's value to be shifted down to the bottom of the mantissa (to make
 * room for the large number being added in). Since the mantissa is, at a
 * given moment in time, a fixed point integer itself, one can convert a
 * float to various fixed point representations by moving around the point
 * of a floating point number through arithmetic operations. This behavior
 * is reliable on most modern platforms as it is mandated by the IEEE-754
 * standard for floating point arithmetic.
 *
 * For our purposes, a "magic number" must be carefully selected that is
 * both large enough to produce the desired point-shifting effect, and also
 * has no lower bits in its representation that would interfere with our
 * value at the bottom of the mantissa. The magic number is calculated as
 * follows:
 *
 *          (2 ^ (MANTISSA_SIZE - FRACTIONAL_SIZE)) * 1.5
 *
 * where in our case:
 *  - MANTISSA_SIZE for 64-bit doubles is 52
 *  - FRACTIONAL_SIZE for 16.16 fixed point is 16
 *
 * Although this approach provides a very large speedup of this function
 * on a wide-array of systems, it does come with two caveats:
 *
 * 1) It uses banker's rounding as opposed to arithmetic rounding.
 * 2) It doesn't function properly if the FPU is in single-precision
 *    mode.
 */

#include <stdint.h>

/* The 16.16 number must always be available */
#define CAIRO_MAGIC_NUMBER_FIXED_16_16 (103079215104.0)

typedef int32_t		cairo_fixed_16_16_t;

static inline cairo_fixed_16_16_t
_cairo_fixed_16_16_from_double (double d)
{
    union {
        double d;
        int32_t i[2];
    } u;

    u.d = d + CAIRO_MAGIC_NUMBER_FIXED_16_16;
#ifdef FLOAT_WORDS_BIGENDIAN
    return u.i[1];
#else
    return u.i[0];
#endif
}

