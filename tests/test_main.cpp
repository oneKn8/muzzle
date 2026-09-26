// Unit tests. No framework dependency on purpose: the same checks should be easy to run
// anywhere the core builds, and a 30-line harness is enough for that.
#include <cstdio>
#include <cstring>
#include <vector>

#include "muzzle/feetech.hpp"
#include "muzzle/frame_parser.hpp"
#include "muzzle/sample_traffic.hpp"

using namespace muzzle::feetech;
using Event = FrameParser::Event;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

struct Parsed {
    std::uint8_t id, len, code, chk;
    std::vector<std::uint8_t> params;
};

// Feeds a whole buffer and collects every valid frame.
std::vector<Parsed> parse_all(FrameParser& p, const std::vector<std::uint8_t>& bytes) {
    std::vector<Parsed> out;
    for (std::uint8_t b : bytes) {
        if (p.feed(b) == Event::kFrame) {
            const FrameView& f = p.frame();
            out.push_back({f.id, f.len, f.code, f.chk,
                           std::vector<std::uint8_t>(f.params, f.params + f.param_count)});
        }
    }
    return out;
}

std::vector<std::uint8_t> cat(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// The official examples from Feetech's protocol manual (docs/feetech-protocol.md §2).
const std::vector<std::uint8_t> kSyncReadReq = {0xFF, 0xFF, 0xFE, 0x06, 0x82, 0x38, 0x08, 0x01, 0x02, 0x36};
const std::vector<std::uint8_t> kReply1 = {0xFF, 0xFF, 0x01, 0x0A, 0x00, 0x00, 0x08, 0x00,
                                           0x00, 0x00, 0x00, 0x79, 0x1E, 0x55};
const std::vector<std::uint8_t> kReply2 = {0xFF, 0xFF, 0x02, 0x0A, 0x00, 0xFF, 0x07, 0x00,
                                           0x00, 0x00, 0x00, 0x77, 0x23, 0x53};
const std::vector<std::uint8_t> kAction = {0xFF, 0xFF, 0xFE, 0x02, 0x05, 0xFA};
const std::vector<std::uint8_t> kPing3 = {0xFF, 0xFF, 0x03, 0x02, 0x01, 0xF9};

void test_official_checksums() {
    CHECK(checksum(0xFE, 0x06, 0x82, kSyncReadReq.data() + 5, 4) == 0x36);
    CHECK(checksum(0x01, 0x0A, 0x00, kReply1.data() + 5, 8) == 0x55);
    CHECK(checksum(0x02, 0x0A, 0x00, kReply2.data() + 5, 8) == 0x53);
    CHECK(checksum(0xFE, 0x02, 0x05, nullptr, 0) == 0xFA);
    CHECK(checksum(0x03, 0x02, 0x01, nullptr, 0) == 0xF9);
}

void test_official_stream() {
    FrameParser p;
    const auto frames = parse_all(p, cat({kSyncReadReq, kReply1, kReply2}));
    CHECK(frames.size() == 3);
    if (frames.size() != 3) return;
    CHECK(frames[0].id == 0xFE && frames[0].code == inst::kSyncRead);
    CHECK((frames[0].params == std::vector<std::uint8_t>{0x38, 0x08, 0x01, 0x02}));
    CHECK(frames[1].id == 0x01 && frames[1].code == 0x00 && frames[1].params.size() == 8);
    // Reply 2 carries an FF data byte (position 0x07FF). Inside a frame it is data, not a header.
    CHECK(frames[2].id == 0x02 && frames[2].params[0] == 0xFF && frames[2].params[1] == 0x07);
    CHECK(read_u16_le(frames[2].params.data()) == 2047);
    CHECK(p.stats().frames == 3 && p.stats().skipped_bytes == 0);
}

void test_encoders_match_manual() {
    std::uint8_t buf[64];
    const std::uint8_t ids[2] = {1, 2};
    const std::size_t n = encode_sync_read(buf, sizeof buf, 0x38, 0x08, ids, 2);
    CHECK(n == kSyncReadReq.size());
    CHECK(std::memcmp(buf, kSyncReadReq.data(), n) == 0);

    CHECK(encode_frame(buf, sizeof buf, 0xFE, inst::kAction, nullptr, 0) == kAction.size());
    CHECK(std::memcmp(buf, kAction.data(), kAction.size()) == 0);
}

void test_sync_write_roundtrip() {
    std::uint8_t buf[64];
    const std::uint8_t ids[6] = {1, 2, 3, 4, 5, 6};
    std::uint8_t data[12];
    for (int i = 0; i < 6; ++i) write_u16_le(data + 2 * i, encode_sign_magnitude(1000 + i, 15));
    const std::size_t n = encode_sync_write(buf, sizeof buf, reg::kGoalPosition, 2, ids, 6, data);
    CHECK(n == 26);  // 6 bytes of framing + addr, width + 6 blocks of (id, lo, hi)
    FrameParser p;
    const auto frames = parse_all(p, std::vector<std::uint8_t>(buf, buf + n));
    CHECK(frames.size() == 1);
    if (frames.size() != 1) return;
    CHECK(frames[0].id == kBroadcastId && frames[0].code == inst::kSyncWrite);
    CHECK(frames[0].params[0] == reg::kGoalPosition && frames[0].params[1] == 2);
    for (int i = 0; i < 6; ++i) {
        const std::uint8_t* block = frames[0].params.data() + 2 + 3 * i;
        CHECK(block[0] == ids[i]);
        CHECK(decode_sign_magnitude(read_u16_le(block + 1), 15) == 1000 + i);
    }
}

void test_garbage_before_frame() {
    FrameParser p;
    const auto frames = parse_all(p, cat({{0x00, 0x12, 0xFF, 0x34, 0x99}, kPing3}));
    CHECK(frames.size() == 1 && frames[0].id == 3);
    CHECK(p.stats().skipped_bytes == 5);
}

void test_extra_header_bytes() {
    FrameParser p;
    const auto frames = parse_all(p, cat({{0xFF, 0xFF, 0xFF}, kPing3}));
    CHECK(frames.size() == 1 && frames[0].id == 3);
    CHECK(p.stats().skipped_bytes == 3);
}

void test_bad_checksum_then_recovery() {
    auto corrupted = kReply1;
    corrupted[7] ^= 0x10;  // flip one data bit
    FrameParser p;
    std::vector<Event> events;
    for (std::uint8_t b : cat({corrupted, kReply2})) {
        const Event e = p.feed(b);
        if (e != Event::kNone) events.push_back(e);
    }
    CHECK(events.size() == 2);
    CHECK(events.size() == 2 && events[0] == Event::kBadChecksum && events[1] == Event::kFrame);
    CHECK(p.stats().bad_checksum == 1 && p.stats().frames == 1);
}

void test_bad_length_values() {
    for (std::uint8_t len : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{251}, std::uint8_t{0xFE}}) {
        FrameParser p;
        Event last = Event::kNone;
        for (std::uint8_t b : {std::uint8_t{0xFF}, std::uint8_t{0xFF}, std::uint8_t{0x01}, len}) last = p.feed(b);
        CHECK(last == Event::kBadLength);
        CHECK(!p.in_frame());
    }
}

void test_bad_length_ff_starts_next_header() {
    // A lone header candidate whose LEN is FF, directly followed by a real frame whose
    // first header byte is shared with that LEN byte's successor.
    FrameParser p;
    const auto frames = parse_all(p, cat({{0xFF, 0xFF, 0x05}, kPing3}));
    CHECK(frames.size() == 1 && frames[0].id == 3);
    CHECK(p.stats().bad_length == 1);
}

void test_max_length_frame() {
    std::vector<std::uint8_t> params(kMaxParams);
    for (std::size_t i = 0; i < params.size(); ++i) params[i] = static_cast<std::uint8_t>(i);
    std::vector<std::uint8_t> buf(kMaxFrameBytes + 8);
    const std::size_t n = encode_frame(buf.data(), buf.size(), 7, inst::kWrite, params.data(), params.size());
    CHECK(n == kMaxFrameBytes);
    buf.resize(n);
    FrameParser p;
    const auto frames = parse_all(p, buf);
    CHECK(frames.size() == 1 && frames[0].len == kMaxLen && frames[0].params == params);

    std::uint8_t small[8];
    CHECK(encode_frame(small, sizeof small, 1, inst::kWrite, params.data(), params.size()) == 0);
    CHECK(encode_frame(buf.data(), buf.size(), 1, inst::kWrite, params.data(), kMaxParams + 1) == 0);
}

void test_encode_never_writes_past_cap() {
    std::uint8_t buf[16];
    std::memset(buf, 0xAA, sizeof buf);
    const std::uint8_t params[4] = {1, 2, 3, 4};
    CHECK(encode_frame(buf, 9, 1, inst::kWrite, params, 4) == 0);  // needs 10
    for (std::uint8_t b : buf) CHECK(b == 0xAA);
    CHECK(encode_frame(buf, 10, 1, inst::kWrite, params, 4) == 10);
    for (std::size_t i = 10; i < sizeof buf; ++i) CHECK(buf[i] == 0xAA);
}

void test_truncated_frame_and_abandon() {
    const std::vector<std::uint8_t> truncated(kReply1.begin(), kReply1.begin() + 7);

    // Without abandon(), the cut-off frame eats the start of the next frame: documented.
    FrameParser eaten;
    CHECK(parse_all(eaten, cat({truncated, kPing3})).empty());

    // With abandon() at the idle gap, the next frame survives.
    FrameParser p;
    parse_all(p, truncated);
    CHECK(p.in_frame());
    p.abandon();
    CHECK(!p.in_frame());
    const auto frames = parse_all(p, kPing3);
    CHECK(frames.size() == 1 && frames[0].id == 3);
    CHECK(p.stats().abandoned == 1);
}

void test_reset_clears_everything() {
    FrameParser p;
    parse_all(p, cat({{0x00}, kPing3, {0xFF, 0xFF}}));
    p.reset();
    CHECK(p.stats().frames == 0 && p.stats().skipped_bytes == 0 && !p.in_frame());
    CHECK(parse_all(p, kPing3).size() == 1);
}

void test_sign_magnitude() {
    CHECK(encode_sign_magnitude(2047, 15) == 2047);
    CHECK(encode_sign_magnitude(-2047, 15) == (0x8000 | 2047));
    CHECK(decode_sign_magnitude(0x8000 | 2047, 15) == -2047);
    CHECK(decode_sign_magnitude(encode_sign_magnitude(-300, 10), 10) == -300);
    CHECK(encode_sign_magnitude(INT32_MIN, 15) == 0xFFFF);  // clamped magnitude, negative
    CHECK(encode_sign_magnitude(5000, 10) == 1023);          // clamps to the field width
    CHECK(decode_sign_magnitude(0, 15) == 0);
}

void test_sample_traffic_parses_exactly() {
    std::vector<std::uint8_t> buf(muzzle::sample::kMaxStepBytes * 50);
    std::size_t n = 0;
    for (std::uint32_t s = 0; s < 50; ++s)
        n += muzzle::sample::append_control_step(buf.data() + n, buf.size() - n, s);
    buf.resize(n);
    FrameParser p;
    CHECK(parse_all(p, buf).size() == 50 * muzzle::sample::kFramesPerStep);
    CHECK(p.stats().bad_checksum == 0 && p.stats().bad_length == 0 && p.stats().skipped_bytes == 0);
}

}  // namespace

int main() {
    test_official_checksums();
    test_official_stream();
    test_encoders_match_manual();
    test_sync_write_roundtrip();
    test_garbage_before_frame();
    test_extra_header_bytes();
    test_bad_checksum_then_recovery();
    test_bad_length_values();
    test_bad_length_ff_starts_next_header();
    test_max_length_frame();
    test_encode_never_writes_past_cap();
    test_truncated_frame_and_abandon();
    test_reset_clears_everything();
    test_sign_magnitude();
    test_sample_traffic_parses_exactly();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
