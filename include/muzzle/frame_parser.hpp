// Streaming parser for Feetech bus frames, one byte at a time, constant work per byte.
//
// Why byte-at-a-time: on the microcontroller each byte is handed over from the UART as it
// lands, and whatever decides about a frame has to keep pace with a new byte every 10 us.
// So feed() does a fixed amount of work per call, never backtracks, and never allocates.
//
// Resynchronisation rules (the part every hand-rolled serial parser gets subtly wrong):
//   * FF FF FF ... : extra FF bytes shift the header, the last two FFs are the header.
//   * LEN < 2 or LEN > 250 : not a frame. If that LEN byte was itself FF it may be the
//     first byte of the next header, so scanning resumes as if one FF had been seen.
//   * Bad checksum : the frame is reported and dropped, scanning restarts at the next byte.
//     No backtracking into the dropped bytes; that keeps the per-byte cost constant.
//   * A frame cut off mid-way would swallow the next frame's bytes as its own. The owner
//     is expected to call abandon() when the line goes idle mid-frame (the UART reports
//     that as an RX timeout), which drops the partial frame.
#pragma once

#include <cstddef>
#include <cstdint>

#include "muzzle/feetech.hpp"

namespace muzzle::feetech {

// A decoded frame. `params` points into the parser's buffer and is only valid until the
// next call to feed().
struct FrameView {
    std::uint8_t id = 0;
    std::uint8_t len = 0;
    std::uint8_t code = 0;  // instruction, or error byte on a status reply
    std::uint8_t chk = 0;   // checksum byte as received
    const std::uint8_t* params = nullptr;
    std::size_t param_count = 0;
};

class FrameParser {
public:
    enum class Event : std::uint8_t {
        kNone,         // byte consumed, nothing completed
        kFrame,        // frame() holds a frame with a valid checksum
        kBadChecksum,  // frame() holds the rejected frame, for diagnostics only
        kBadLength,    // a header was followed by an impossible LEN
    };

    struct Stats {
        std::uint32_t frames = 0;
        std::uint32_t bad_checksum = 0;
        std::uint32_t bad_length = 0;
        std::uint32_t abandoned = 0;
        std::uint32_t skipped_bytes = 0;  // bytes that were not part of any candidate frame
    };

    Event feed(std::uint8_t byte);

    const FrameView& frame() const { return view_; }
    const Stats& stats() const { return stats_; }

    // True once a full header has been seen and the frame is not finished yet.
    bool in_frame() const { return state_ >= State::kId; }

    // The line went idle mid-frame: drop the partial frame and start scanning again.
    void abandon();

    // Back to power-on state, stats included.
    void reset();

private:
    enum class State : std::uint8_t { kSeekFirst, kSeekSecond, kId, kLen, kCode, kParams, kChecksum };

    State state_ = State::kSeekFirst;
    std::uint8_t id_ = 0;
    std::uint8_t len_ = 0;
    std::uint8_t code_ = 0;
    std::uint32_t sum_ = 0;
    std::size_t expected_ = 0;
    std::size_t got_ = 0;
    std::uint8_t params_[kMaxParams] = {};
    FrameView view_{};
    Stats stats_{};
};

}  // namespace muzzle::feetech
