#pragma once

// ninfer::ops::detail - private launch prototype for token_mask.

#include <cstdint>

#include <cuda_bf16.h>
#include <cuda_runtime.h>

namespace ninfer::ops::detail {

void token_mask_launch(__nv_bfloat16* logits, const std::int32_t* masks,
                       const std::int32_t* enabled, std::int32_t token_domain,
                       std::int32_t physical_rows, std::int32_t columns, std::int32_t words,
                       cudaStream_t stream);

} // namespace ninfer::ops::detail
