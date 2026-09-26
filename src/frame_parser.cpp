#include "muzzle/frame_parser.hpp"

#include "muzzle/platform.hpp"

namespace muzzle::feetech {

MUZZLE_HOT FrameParser::Event FrameParser::feed(std::uint8_t b) {
    switch (state_) {
        case State::kSeekFirst:
            if (b == kHeaderByte) {
                state_ = State::kSeekSecond;
            } else {
                ++stats_.skipped_bytes;
            }
            return Event::kNone;

        case State::kSeekSecond:
            if (b == kHeaderByte) {
                state_ = State::kId;
            } else {
                stats_.skipped_bytes += 2;  // the lone FF and this byte
                state_ = State::kSeekFirst;
            }
            return Event::kNone;

        case State::kId:
            if (b == kHeaderByte) {
                ++stats_.skipped_bytes;  // FF FF FF: the oldest FF was not a header byte
                return Event::kNone;
            }
            id_ = b;
            state_ = State::kLen;
            return Event::kNone;

        case State::kLen:
            if (b < kMinLen || b > kMaxLen) {
                ++stats_.bad_length;
                state_ = (b == kHeaderByte) ? State::kSeekSecond : State::kSeekFirst;
                return Event::kBadLength;
            }
            len_ = b;
            expected_ = static_cast<std::size_t>(b) - 2;
            state_ = State::kCode;
            return Event::kNone;

        case State::kCode:
            code_ = b;
            sum_ = static_cast<std::uint32_t>(id_) + len_ + code_;
            got_ = 0;
            state_ = expected_ ? State::kParams : State::kChecksum;
            return Event::kNone;

        case State::kParams:
            params_[got_++] = b;
            sum_ += b;
            if (got_ == expected_) state_ = State::kChecksum;
            return Event::kNone;

        case State::kChecksum: {
            view_.id = id_;
            view_.len = len_;
            view_.code = code_;
            view_.chk = b;
            view_.params = params_;
            view_.param_count = expected_;
            state_ = State::kSeekFirst;
            if (b == checksum_from_sum(sum_)) {
                ++stats_.frames;
                return Event::kFrame;
            }
            ++stats_.bad_checksum;
            return Event::kBadChecksum;
        }
    }
    return Event::kNone;  // unreachable; keeps -Wreturn-type quiet on older GCC
}

void FrameParser::abandon() {
    if (state_ != State::kSeekFirst) {
        if (in_frame()) ++stats_.abandoned;
        else ++stats_.skipped_bytes;  // a single dangling FF
    }
    state_ = State::kSeekFirst;
}

void FrameParser::reset() {
    *this = FrameParser{};
}

}  // namespace muzzle::feetech
