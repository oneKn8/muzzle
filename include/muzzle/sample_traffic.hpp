// A synthetic but protocol-exact control step, shaped like what LeRobot sends an SO-101
// every frame: SYNC_READ of Present_Position for servos 1-6, the six status replies, then
// one SYNC_WRITE of Goal_Position. Shared by the host bench, the tests, and the ESP32-C3
// bench so every number is measured on the same bytes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "muzzle/feetech.hpp"

namespace muzzle::sample {

inline constexpr std::uint8_t kArmIds[6] = {1, 2, 3, 4, 5, 6};

// Frames produced per control step: 1 request + 6 replies + 1 goal write.
inline constexpr std::size_t kFramesPerStep = 8;

// Bytes in one step: request 14, replies 6 x 8, goal write 26.
inline constexpr std::size_t kMaxStepBytes = 14 + 6 * 8 + 26;

// Appends one control step to `out`. `step` varies the positions so repeated steps are
// not byte-identical. Returns bytes written, or 0 if `cap` is too small.
inline std::size_t append_control_step(std::uint8_t* out, std::size_t cap, std::uint32_t step) {
    using namespace muzzle::feetech;
    if (cap < kMaxStepBytes) return 0;
    std::size_t k = 0;

    k += encode_sync_read(out + k, cap - k, reg::kPresentPosition, 2, kArmIds, 6);

    std::uint8_t goals[12];
    for (std::size_t i = 0; i < 6; ++i) {
        // Positions sweep around mid-range (2048) so both bytes change over time.
        const auto pos = static_cast<std::int32_t>(2048 + ((step * 7u + i * 311u) % 1024u)) - 512;
        std::uint8_t data[2];
        write_u16_le(data, encode_sign_magnitude(pos, 15));
        // Status reply: FF FF ID LEN ERR pos_lo pos_hi CHK, error byte 0.
        k += encode_frame(out + k, cap - k, kArmIds[i], 0x00, data, 2);
        write_u16_le(goals + 2 * i, encode_sign_magnitude(pos + 3, 15));
    }

    k += encode_sync_write(out + k, cap - k, reg::kGoalPosition, 2, kArmIds, 6, goals);
    return k;
}

}  // namespace muzzle::sample
