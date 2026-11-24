#pragma once
#include <cstddef>
#include <span>

namespace clipping {
struct H264AccessUnit {
    bool annex_b = false;
    bool idr = false;
    bool sps = false;
    bool pps = false;
};
// Hardware Media Foundation H.264 output uses Annex B access units. Validate
// boundaries rather than equating every encoder CleanPoint/I frame with an IDR.
constexpr H264AccessUnit inspect_h264(std::span<const std::byte> bytes) {
    H264AccessUnit result;
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] != std::byte{0} || bytes[i + 1] != std::byte{0}) continue;
        std::size_t header = i + 3;
        if (bytes[i + 2] == std::byte{1}) { /* three-byte start code */ }
        else if (i + 4 < bytes.size() && bytes[i + 2] == std::byte{0} && bytes[i + 3] == std::byte{1}) header = i + 4;
        else continue;
        const auto value = std::to_integer<unsigned>(bytes[header]);
        if (value & 0x80U) continue; // forbidden_zero_bit
        result.annex_b = true;
        const auto type = value & 0x1fU;
        result.idr |= type == 5;
        result.sps |= type == 7;
        result.pps |= type == 8;
        i = header;
    }
    return result;
}
} // namespace clipping
