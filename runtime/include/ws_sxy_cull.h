#pragma once

#include <stdint.h>

static inline int psx_ws_sxy_x_lower_impl(uint32_t sx_shifted, int32_t margin)
{
    const int32_t x = (int32_t)sx_shifted >> 16;
    return x < (margin > 0 ? -margin : 0);
}

static inline int psx_ws_sxy_tri_impl(uint32_t sxy0, uint32_t sxy1,
                                      uint32_t sxy2, int32_t margin)
{
    const uint32_t common = sxy0 & sxy1 & sxy2;
    if (common & 0x80000000u) return 1;
    if (!(common & 0x00008000u)) return 0;
    if (margin <= 0) return 1;
    const int32_t left = -margin;
    return (int16_t)(sxy0 & 0xffffu) < left &&
           (int16_t)(sxy1 & 0xffffu) < left &&
           (int16_t)(sxy2 & 0xffffu) < left;
}

static inline int psx_ws_sxy_quad_impl(uint32_t sxy0, uint32_t sxy1,
                                       uint32_t sxy2, uint32_t sxy3,
                                       int32_t margin)
{
    const uint32_t common = sxy0 & sxy1 & sxy2 & sxy3;
    if (common & 0x80000000u) return 1;
    if (!(common & 0x00008000u)) return 0;
    if (margin <= 0) return 1;
    const int32_t left = -margin;
    return (int16_t)(sxy0 & 0xffffu) < left &&
           (int16_t)(sxy1 & 0xffffu) < left &&
           (int16_t)(sxy2 & 0xffffu) < left &&
           (int16_t)(sxy3 & 0xffffu) < left;
}
