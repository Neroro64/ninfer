// Implements: include/ninfer/ops/token_mask.h
// Match: contiguous BF16 [physical_rows,T] logits, I32 [words,T] masks, I32 [T] enabled.
// One block column per batch row; disabled rows exit before any logits traffic.

#include <cstdint>

#include <cuda_bf16.h>
#include <cuda_runtime.h>
#include <math_constants.h>

namespace {

constexpr int kTokenMaskBlock = 256;

} // namespace

__global__ __launch_bounds__(kTokenMaskBlock) void token_mask_kernel(
    __nv_bfloat16* __restrict__ logits, const std::int32_t* __restrict__ masks,
    const std::int32_t* __restrict__ enabled, std::int32_t token_domain,
    std::int32_t physical_rows, std::int32_t words) {
    const std::int32_t t = static_cast<std::int32_t>(blockIdx.y);
    if (enabled[t] == 0) { return; }

    const std::int64_t base      = static_cast<std::int64_t>(t) * physical_rows;
    const std::int32_t* row_bits = masks + static_cast<std::int64_t>(t) * words;

    const std::int32_t stride = static_cast<std::int32_t>(gridDim.x) * kTokenMaskBlock;
    for (std::int32_t v = static_cast<std::int32_t>(blockIdx.x) * kTokenMaskBlock + threadIdx.x;
         v < token_domain; v += stride) {
        const std::uint32_t bits = static_cast<std::uint32_t>(row_bits[v >> 5]);
        if (((bits >> (v & 31)) & 1U) == 0U) {
            logits[base + v] = __float2bfloat16(-CUDART_INF_F);
        }
    }
}
