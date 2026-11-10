/* ctypes wrapper around richgel999/bc7enc16 (MIT / public domain).

   Build (the two upstream files are fetched separately, see ps5_assets.py):
     gcc -O3 -fPIC -shared -fopenmp -o libbc7enc16.so \
         bc7enc16.c bc7enc16_wrapper.c -lm

   Exposes one call that turns a whole RGBA image into BC7 blocks. PS5's
   sce_sys/pic0.dds and pic1.dds are 3840x2160 DXGI_FORMAT_BC7_UNORM, so the
   output is exactly width*height bytes (16 bytes per 4x4 block). */
#include <stdint.h>
#include <stddef.h>
#include "bc7enc16.h"

int swr_bc7_encode(const uint8_t *rgba, int w, int h, uint8_t *out, int uber_level)
{
    if (!rgba || !out || w <= 0 || h <= 0 || (w & 3) || (h & 3)) return -1;
    bc7enc16_compress_block_init();
    bc7enc16_compress_block_params p;
    /* Same defaults as bc7enc16_compress_block_params_init(), spelled out so we
       do not depend on the header's C inline semantics when linking a .so. */
    p.m_max_partitions_mode1 = BC7ENC16_MAX_PARTITIONS1;
    p.m_weights[0] = 128; p.m_weights[1] = 64;
    p.m_weights[2] = 16;  p.m_weights[3] = 32;
    p.m_uber_level = (uint32_t)(uber_level < 0 ? 0 :
                                (uber_level > BC7ENC16_MAX_UBER_LEVEL ? BC7ENC16_MAX_UBER_LEVEL : uber_level));
    p.m_perceptual = BC7ENC16_TRUE;
    p.m_try_least_squares = BC7ENC16_TRUE;
    p.m_mode1_partition_estimation_filterbank = BC7ENC16_TRUE;

    const int bw = w >> 2;
    const int bh = h >> 2;
    #pragma omp parallel for schedule(static)
    for (int by = 0; by < bh; ++by) {
        uint8_t px[16 * 4];
        for (int bx = 0; bx < bw; ++bx) {
            for (int y = 0; y < 4; ++y) {
                const uint8_t *src = rgba + ((size_t)(by * 4 + y) * w + bx * 4) * 4;
                for (int x = 0; x < 4; ++x) {
                    uint8_t *dst = px + (y * 4 + x) * 4;
                    dst[0] = src[x * 4 + 0];
                    dst[1] = src[x * 4 + 1];
                    dst[2] = src[x * 4 + 2];
                    dst[3] = src[x * 4 + 3];
                }
            }
            bc7enc16_compress_block(out + ((size_t)by * bw + bx) * BC7ENC16_BLOCK_SIZE,
                                    px, &p);
        }
    }
    return 0;
}
