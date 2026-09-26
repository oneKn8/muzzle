// Feetech STS/SMS serial bus protocol: frame constants, checksum, and frame builders.
//
// Everything here is sourced from docs/feetech-protocol.md, which cites the official
// Feetech protocol manual and memory table plus the LeRobot driver. Frame shape:
//
//   FF FF ID LEN CODE P1..PN CHK      LEN = N + 2, total frame = LEN + 4
//   CHK = ~(ID + LEN + CODE + sum(P)) & 0xFF
//
// CODE is the instruction on a host->servo frame and the error byte on a status reply;
// the framing is identical in both directions, which is why one parser serves both.
//
// C++17, no heap, no exceptions: this header has to build unchanged on the laptop and
// with the ESP32-C3's GCC 8.4 toolchain.
#pragma once

#include <cstddef>
#include <cstdint>

namespace muzzle::feetech {

inline constexpr std::uint8_t kHeaderByte = 0xFF;
inline constexpr std::uint8_t kBroadcastId = 0xFE;
inline constexpr std::uint8_t kMaxUnicastId = 0xFD;

// LEN counts CODE + params + CHK. 2 is an empty frame (PING, ACTION). The reference SDK
// refuses anything above 250, so a larger LEN is treated as line noise, not a frame.
inline constexpr std::uint8_t kMinLen = 2;
inline constexpr std::uint8_t kMaxLen = 250;
inline constexpr std::size_t kMaxParams = kMaxLen - 2;
inline constexpr std::size_t kMaxFrameBytes = kMaxLen + 4;

// One byte is 10 bits on the wire (8N1), so 10 microseconds at the default 1 Mbps.
inline constexpr std::uint32_t kDefaultBaud = 1000000;

namespace inst {
inline constexpr std::uint8_t kPing = 0x01;
inline constexpr std::uint8_t kRead = 0x02;
inline constexpr std::uint8_t kWrite = 0x03;
inline constexpr std::uint8_t kRegWrite = 0x04;
inline constexpr std::uint8_t kAction = 0x05;
inline constexpr std::uint8_t kParamRestore = 0x06;
inline constexpr std::uint8_t kReboot = 0x08;
inline constexpr std::uint8_t kParamBackup = 0x09;
inline constexpr std::uint8_t kStateReset = 0x0A;
inline constexpr std::uint8_t kPositionCal = 0x0B;
inline constexpr std::uint8_t kSyncRead = 0x82;
inline constexpr std::uint8_t kSyncWrite = 0x83;
}  // namespace inst

// The registers this project reads or reasons about. Addresses from the Feetech STS
// memory table; where LeRobot's table disagrees, Feetech wins (see the protocol doc).
namespace reg {
inline constexpr std::uint8_t kModelNumber = 3;       // 2 bytes, STS3215 = 777
inline constexpr std::uint8_t kId = 5;
inline constexpr std::uint8_t kBaudRate = 6;
inline constexpr std::uint8_t kMinPositionLimit = 9;  // 2 bytes
inline constexpr std::uint8_t kMaxPositionLimit = 11; // 2 bytes
inline constexpr std::uint8_t kHomingOffset = 31;     // 2 bytes, sign bit 11
inline constexpr std::uint8_t kOperatingMode = 33;
inline constexpr std::uint8_t kTorqueEnable = 40;
inline constexpr std::uint8_t kAcceleration = 41;
inline constexpr std::uint8_t kGoalPosition = 42;     // 2 bytes, sign bit 15
inline constexpr std::uint8_t kGoalTime = 44;         // 2 bytes
inline constexpr std::uint8_t kGoalVelocity = 46;     // 2 bytes, sign bit 15
inline constexpr std::uint8_t kTorqueLimit = 48;      // 2 bytes
inline constexpr std::uint8_t kLock = 55;
inline constexpr std::uint8_t kPresentPosition = 56;  // 2 bytes, sign bit 15
inline constexpr std::uint8_t kPresentVelocity = 58;  // 2 bytes, sign bit 15
inline constexpr std::uint8_t kPresentLoad = 60;      // 2 bytes, sign bit 10
inline constexpr std::uint8_t kPresentVoltage = 62;
inline constexpr std::uint8_t kPresentTemperature = 63;
}  // namespace reg

inline constexpr std::uint16_t kTicksPerRev = 4096;

// Status-reply error bits (same layout as the Status register).
namespace err {
inline constexpr std::uint8_t kVoltage = 1u << 0;
inline constexpr std::uint8_t kEncoder = 1u << 1;
inline constexpr std::uint8_t kTemperature = 1u << 2;
inline constexpr std::uint8_t kCurrent = 1u << 3;
inline constexpr std::uint8_t kLoad = 1u << 5;
}  // namespace err

// Running sum over ID, LEN, CODE and params, folded into the checksum at the end.
// Kept as a separate step so a streaming consumer can accumulate byte by byte.
constexpr std::uint8_t checksum_from_sum(std::uint32_t sum) {
    return static_cast<std::uint8_t>(~sum & 0xFFu);
}

constexpr std::uint8_t checksum(std::uint8_t id, std::uint8_t len, std::uint8_t code,
                                const std::uint8_t* params, std::size_t n) {
    std::uint32_t sum = static_cast<std::uint32_t>(id) + len + code;
    for (std::size_t i = 0; i < n; ++i) sum += params[i];
    return checksum_from_sum(sum);
}

// Sign-magnitude as the servo uses it: magnitude in the low bits, direction in `sign_bit`.
constexpr std::uint16_t encode_sign_magnitude(std::int32_t value, unsigned sign_bit) {
    const std::uint32_t mask = (1u << sign_bit) - 1u;
    // Negate in unsigned arithmetic so INT32_MIN does not overflow.
    const std::uint32_t mag = value < 0 ? 0u - static_cast<std::uint32_t>(value)
                                        : static_cast<std::uint32_t>(value);
    const std::uint32_t clamped = mag > mask ? mask : mag;
    return static_cast<std::uint16_t>(clamped | (value < 0 ? (1u << sign_bit) : 0u));
}

constexpr std::int32_t decode_sign_magnitude(std::uint16_t raw, unsigned sign_bit) {
    const std::uint32_t mask = (1u << sign_bit) - 1u;
    const std::int32_t mag = static_cast<std::int32_t>(raw & mask);
    return (raw & (1u << sign_bit)) ? -mag : mag;
}

constexpr std::uint16_t read_u16_le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

constexpr void write_u16_le(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

// Writes one complete frame into `out`. Returns the byte count, or 0 if the frame would
// not fit in `cap` or exceeds the protocol's LEN limit. Never writes past `cap`.
std::size_t encode_frame(std::uint8_t* out, std::size_t cap, std::uint8_t id, std::uint8_t code,
                         const std::uint8_t* params, std::size_t n);

// Convenience builders for the frames LeRobot sends every control step. Each returns
// the frame length or 0 on overflow.
std::size_t encode_read(std::uint8_t* out, std::size_t cap, std::uint8_t id, std::uint8_t addr,
                        std::uint8_t count);

std::size_t encode_sync_read(std::uint8_t* out, std::size_t cap, std::uint8_t addr,
                             std::uint8_t count, const std::uint8_t* ids, std::size_t n_ids);

// `data` holds n_ids blocks of `width` bytes, one block per id, in id order.
std::size_t encode_sync_write(std::uint8_t* out, std::size_t cap, std::uint8_t addr,
                              std::uint8_t width, const std::uint8_t* ids, std::size_t n_ids,
                              const std::uint8_t* data);

}  // namespace muzzle::feetech
