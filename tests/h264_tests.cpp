#include "clipping/h264.hpp"
#include <array>
#include <initializer_list>

constexpr clipping::H264AccessUnit inspect(std::initializer_list<unsigned char> values) {
    std::array<std::byte, 32> bytes{};
    std::size_t i = 0;
    for (auto value : values) bytes[i++] = static_cast<std::byte>(value);
    return clipping::inspect_h264(std::span<const std::byte>(bytes.data(), values.size()));
}
static_assert(!inspect({}).annex_b);
static_assert(!inspect({0,0,1}).annex_b); // truncated header
static_assert(!inspect({0,0,0,1}).annex_b);
static_assert(inspect({0,0,1,0x65,0xaa}).idr);
static_assert(inspect({0,0,0,1,0x65}).idr);
static_assert(!inspect({0,0,1,0x61,0xaa}).idr); // non-IDR slice
static_assert(!inspect({0,0,1,0xe5}).idr); // invalid forbidden bit
static_assert(!inspect({0,0,3,1,0x65}).idr); // escaped payload, not a start code
constexpr auto unit = inspect({0,0,0,1,0x67,0xaa,0,0,1,0x68,0xbb,0,0,1,0x65});
static_assert(unit.annex_b && unit.sps && unit.pps && unit.idr);
int main() { return 0; }
