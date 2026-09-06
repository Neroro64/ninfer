// ninfer::ops - token_mask wrapper: public api validation and launcher dispatch.
#include "ninfer/ops/token_mask.h"

#include "ops/launcher/token_mask.h" // detail::token_mask_launch

#include <cstdint>
#include <stdexcept>
#include <string>

namespace ninfer::ops {

void apply_token_mask(Tensor& logits, const Tensor& masks, const Tensor& enabled,
                      std::int32_t token_domain, cudaStream_t stream) {
    if (logits.dtype != DType::BF16) {
        throw std::invalid_argument("token_mask: logits must be BF16");
    }
    if (masks.dtype != DType::I32) {
        throw std::invalid_argument("token_mask: masks must be I32");
    }
    if (enabled.dtype != DType::I32) {
        throw std::invalid_argument("token_mask: enabled must be I32");
    }
    if (logits.ne[2] != 1 || logits.ne[3] != 1) {
        throw std::invalid_argument("token_mask: logits must be rank-2 [physical_rows,T]");
    }
    if (masks.ne[2] != 1 || masks.ne[3] != 1) {
        throw std::invalid_argument("token_mask: masks must be rank-2 [words,T]");
    }
    if (enabled.ne[1] != 1 || enabled.ne[2] != 1 || enabled.ne[3] != 1) {
        throw std::invalid_argument("token_mask: enabled must be rank-1 [T]");
    }
    const std::int32_t columns = logits.ne[1];
    if (enabled.ne[0] != columns || masks.ne[1] != columns) {
        throw std::invalid_argument("token_mask: masks/enabled column counts must equal T");
    }
    if (logits.ne[0] <= 0) {
        throw std::invalid_argument("token_mask: physical rows must be positive");
    }
    if (token_domain <= 0 || token_domain > logits.ne[0]) {
        throw std::invalid_argument("token_mask: token_domain must be in [1, logits.ne[0]]");
    }
    const std::int32_t words = (token_domain + 31) / 32;
    if (masks.ne[0] != words) {
        throw std::invalid_argument("token_mask: masks rows must be ceil(token_domain/32)");
    }
    if (columns == 0) { return; }
    if (!logits.is_contiguous() || !masks.is_contiguous() || !enabled.is_contiguous()) {
        throw std::invalid_argument("token_mask: logits/masks/enabled must be contiguous");
    }
    if (logits.data == nullptr || masks.data == nullptr || enabled.data == nullptr) {
        throw std::invalid_argument("token_mask: logits/masks/enabled data must be non-null");
    }

    detail::token_mask_launch(static_cast<__nv_bfloat16*>(logits.data),
                              static_cast<const std::int32_t*>(masks.data),
                              static_cast<const std::int32_t*>(enabled.data), token_domain,
                              static_cast<std::int32_t>(logits.ne[0]), columns, words, stream);
}

} // namespace ninfer::ops
