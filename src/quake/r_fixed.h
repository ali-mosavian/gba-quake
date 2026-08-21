#ifndef R_FIXED_H
#define R_FIXED_H

/* Named fixed-point operations; arguments must not have side effects. */
/* Float conversions are for constants/tools, never per-frame GBA code. */
#define TO_F8(value)             ((int32_t)((value) * 256.0f))
#define TO_F16(value)            ((int32_t)((value) * 65536.0f))
#define TO_F32(value)            ((int64_t)((value) * 4294967296.0))

#define Q8_ONE                  (1 << 8)
/* Multiplication, not a shift: e1m7's spawn sits at negative x, and shifting
 * a negative value is formally undefined. The compiler emits the same lsl. */
#define Q8_FROM_INT(value)      ((int32_t)(value) * 256)
#define Q8_TO_INT(value)        ((int32_t)((value) >> 8))
#define Q8_MUL(a, b)            ((int32_t)(((int64_t)(a) * (b)) >> 8))

#define Q14_ONE                 (1 << 14)
#define Q14_TO_INT(value)       ((int32_t)((value) >> 14))
#define Q14_MUL(a, b)           ((int32_t)(((int64_t)(a) * (b)) >> 14))
#define Q14_DOT2(a, b, c, d)    ((int32_t)(((int64_t)(a) * (b) + \
                                            (int64_t)(c) * (d)) >> 14))

#define Q16_ONE                 (1 << 16)
#define Q16_TO_INT(value)       ((int32_t)((value) >> 16))
#define Q16_MUL(a, b)           ((int32_t)(((int64_t)(a) * (b)) >> 16))

#endif
