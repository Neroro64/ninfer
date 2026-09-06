#include "ninfer/ops/token_mask.h"
#include "ops/op_tester.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ninfer;
using namespace ninfer::test;

namespace {

constexpr std::uint16_t kBf16NegInf = 0xFF80;

struct MaskCase {
    std::int32_t physical_rows = 0;
    std::int32_t token_domain  = 0;
    std::int32_t columns       = 0;
};

std::vector<std::uint16_t> make_logits(const MaskCase& shape) {
    std::vector<std::uint16_t> logits(static_cast<std::size_t>(shape.physical_rows) *
                                      shape.columns);
    for (std::int32_t t = 0; t < shape.columns; ++t) {
        const std::size_t base = static_cast<std::size_t>(t) * shape.physical_rows;
        for (std::int32_t v = 0; v < shape.physical_rows; ++v) {
            const std::uint32_t mixed =
                static_cast<std::uint32_t>(v) * 1664525u + static_cast<std::uint32_t>(t + 1) * 1013904223u;
            logits[base + static_cast<std::size_t>(v)] =
                f32_to_bf16(-24.0f + static_cast<float>(mixed % 3072u) * (1.0f / 256.0f));
        }
    }
    return logits;
}

// Deterministic mask with known structure: columns alternate between sparse denial, dense
// denial, and alternating patterns.
std::vector<std::int32_t> make_masks(const MaskCase& shape, std::int32_t words,
                                     std::vector<char>& allowed_oracle) {
    std::vector<std::int32_t> masks(words * shape.columns, 0);
    allowed_oracle.assign(static_cast<std::size_t>(shape.token_domain) * shape.columns, 0);

    for (std::int32_t t = 0; t < shape.columns; ++t) {
        const std::size_t word_base = static_cast<std::size_t>(t) * words;
        for (std::int32_t v = 0; v < shape.token_domain; ++v) {
            const std::uint32_t mixed =
                static_cast<std::uint32_t>(v) * 2654435761u + static_cast<std::uint32_t>(t) * 40503u;
            bool allowed = false;
            if (t % 3 == 0) {
                allowed = (mixed % 4u) != 0u; // sparse denial
            } else if (t % 3 == 1) {
                allowed = (mixed % 16u) == 0u; // dense denial
            } else {
                allowed = (v % 2) == 0; // alternating
            }
            if (allowed) {
                masks[word_base + static_cast<std::size_t>(v >> 5)] |=
                    static_cast<std::int32_t>(1u << (v & 31));
                allowed_oracle[static_cast<std::size_t>(t) * shape.token_domain +
                               static_cast<std::size_t>(v)] = 1;
            }
        }
    }
    return masks;
}

int run_case(const MaskCase& shape) {
    const std::int32_t words = (shape.token_domain + 31) / 32;
    const auto logits        = make_logits(shape);
    std::vector<char> allowed_oracle;
    const auto masks = make_masks(shape, words, allowed_oracle);

    // Every third column is disabled and must keep its logits untouched.
    std::vector<int> enabled(static_cast<std::size_t>(shape.columns), 1);
    for (std::int32_t t = 2; t < shape.columns; t += 3) {
        enabled[static_cast<std::size_t>(t)] = 0;
    }

    std::vector<std::uint16_t> expected = logits;
    for (std::int32_t t = 0; t < shape.columns; ++t) {
        if (enabled[static_cast<std::size_t>(t)] == 0) { continue; }
        const std::size_t base = static_cast<std::size_t>(t) * shape.physical_rows;
        for (std::int32_t v = 0; v < shape.token_domain; ++v) {
            if (allowed_oracle[static_cast<std::size_t>(t) * shape.token_domain +
                               static_cast<std::size_t>(v)] == 0) {
                expected[base + static_cast<std::size_t>(v)] = kBf16NegInf;
            }
        }
    }

    GuardedDeviceBuffer device_logits(logits.size() * sizeof(std::uint16_t));
    GuardedDeviceBuffer device_masks(masks.size() * sizeof(std::int32_t));
    GuardedDeviceBuffer device_enabled(enabled.size() * sizeof(std::int32_t));
    device_logits.copy_from_host(logits.data(), logits.size() * sizeof(std::uint16_t));
    device_masks.copy_from_host(masks.data(), masks.size() * sizeof(std::int32_t));
    device_enabled.copy_from_host(enabled.data(), enabled.size() * sizeof(std::int32_t));

    Tensor logits_tensor(device_logits.data(), DType::BF16, {shape.physical_rows, shape.columns});
    Tensor masks_tensor(device_masks.data(), DType::I32, {words, shape.columns});
    Tensor enabled_tensor(device_enabled.data(), DType::I32, {shape.columns});
    ops::apply_token_mask(logits_tensor, masks_tensor, enabled_tensor, shape.token_domain,
                          nullptr);
    cuda_synchronize();

    const auto actual = from_device<std::uint16_t>(device_logits.data(), logits.size());
    const std::string label = "token_mask rows=" + std::to_string(shape.physical_rows) +
                              " domain=" + std::to_string(shape.token_domain) +
                              " T=" + std::to_string(shape.columns);
    int failures = verify_exact(label.c_str(), actual, expected);
    failures += device_logits.verify_guards((label + " logits").c_str());
    failures += device_masks.verify_guards((label + " masks").c_str());
    failures += device_enabled.verify_guards((label + " enabled").c_str());
    return failures;
}

} // namespace

int main() {
    if (cuda_unavailable()) {
        std::cout << "SKIP token_mask (no CUDA device)\n";
        return 77;
    }

    int failures = 0;
    failures += run_case({256, 200, 1});
    failures += run_case({512, 33, 3});
    failures += run_case({128, 1, 2});
    failures += run_case({248320, 248077, 6});
    failures += run_case({131072, 131072, 8});
    std::cout << (failures ? "FAIL" : "OK") << " token_mask\n";
    return failures ? 1 : 0;
}
