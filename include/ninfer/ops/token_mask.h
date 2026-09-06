#pragma once

#include "core/tensor.h"

#include <cstdint>

#include <cuda_runtime.h> // cudaStream_t

namespace ninfer::ops {

/**
 * Applies one allowed-token bitmask per column ahead of sampling or argmax:
 *
 *   for every t with enabled[t] != 0 and every v in [0,token_domain):
 *     logits[v,t] is set to BF16 -inf when bit v of the mask column is clear, and is
 *     left unchanged otherwise.
 *
 * `logits` is contiguous BF16 [physical_rows,T], `masks` is contiguous I32 [words,T] with
 * words = ceil(token_domain/32) little-endian bit words per column, where bit v set means
 * token v is allowed, and `enabled` is contiguous I32 [T]. Columns with enabled[t]==0 are
 * left untouched, so an all-zero `enabled` is a no-op. Physical rows
 * [token_domain,physical_rows) never change. Inputs must not overlap the logits storage.
 * The Op has no workspace and changes no state other than masked logits elements.
 */
void apply_token_mask(Tensor& logits, const Tensor& masks, const Tensor& enabled,
                      std::int32_t token_domain, cudaStream_t stream);

} // namespace ninfer::ops
