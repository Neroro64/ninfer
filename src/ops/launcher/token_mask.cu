// Implements: include/ninfer/ops/token_mask.h
#include "ops/launcher/token_mask.h"

#include "ops/kernel/token_mask.cuh"

#include "core/device.h" // CUDA_CHECK

#include <cstdint>

namespace ninfer::ops::detail {

void token_mask_launch(__nv_bfloat16* logits, const std::int32_t* masks,
                       const std::int32_t* enabled, std::int32_t token_domain,
                       std::int32_t physical_rows, std::int32_t columns, std::int32_t words,
                       cudaStream_t stream) {
    if (columns <= 0 || token_domain <= 0 || words <= 0) { return; }
    const unsigned blocks =
        (static_cast<unsigned>(token_domain) + kTokenMaskBlock - 1) / kTokenMaskBlock;
    const dim3 grid(blocks, static_cast<unsigned>(columns));
    token_mask_kernel<<<grid, kTokenMaskBlock, 0, stream>>>(logits, masks, enabled, token_domain,
                                                            physical_rows, words);
    CUDA_CHECK(cudaGetLastError());
}

} // namespace ninfer::ops::detail
