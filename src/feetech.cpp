#include "muzzle/feetech.hpp"

namespace muzzle::feetech {

std::size_t encode_frame(std::uint8_t* out, std::size_t cap, std::uint8_t id, std::uint8_t code,
                         const std::uint8_t* params, std::size_t n) {
    if (n > kMaxParams) return 0;
    const std::size_t total = n + 6;
    if (total > cap) return 0;
    const auto len = static_cast<std::uint8_t>(n + 2);
    out[0] = kHeaderByte;
    out[1] = kHeaderByte;
    out[2] = id;
    out[3] = len;
    out[4] = code;
    for (std::size_t i = 0; i < n; ++i) out[5 + i] = params[i];
    out[5 + n] = checksum(id, len, code, params, n);
    return total;
}

std::size_t encode_read(std::uint8_t* out, std::size_t cap, std::uint8_t id, std::uint8_t addr,
                        std::uint8_t count) {
    const std::uint8_t p[2] = {addr, count};
    return encode_frame(out, cap, id, inst::kRead, p, 2);
}

std::size_t encode_sync_read(std::uint8_t* out, std::size_t cap, std::uint8_t addr,
                             std::uint8_t count, const std::uint8_t* ids, std::size_t n_ids) {
    if (n_ids + 2 > kMaxParams) return 0;
    std::uint8_t p[kMaxParams];
    p[0] = addr;
    p[1] = count;
    for (std::size_t i = 0; i < n_ids; ++i) p[2 + i] = ids[i];
    return encode_frame(out, cap, kBroadcastId, inst::kSyncRead, p, n_ids + 2);
}

std::size_t encode_sync_write(std::uint8_t* out, std::size_t cap, std::uint8_t addr,
                              std::uint8_t width, const std::uint8_t* ids, std::size_t n_ids,
                              const std::uint8_t* data) {
    const std::size_t n = 2 + n_ids * (1 + static_cast<std::size_t>(width));
    if (n > kMaxParams) return 0;
    std::uint8_t p[kMaxParams];
    p[0] = addr;
    p[1] = width;
    std::size_t k = 2;
    for (std::size_t i = 0; i < n_ids; ++i) {
        p[k++] = ids[i];
        for (std::size_t j = 0; j < width; ++j) p[k++] = data[i * width + j];
    }
    return encode_frame(out, cap, kBroadcastId, inst::kSyncWrite, p, n);
}

}  // namespace muzzle::feetech
