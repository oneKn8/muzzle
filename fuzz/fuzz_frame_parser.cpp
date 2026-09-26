// Differential fuzzing of the streaming parser.
//
// The streaming FrameParser is checked against a second parser written independently in
// a different style (whole-buffer, index-based, no state machine). Both implement the
// resync rules documented in frame_parser.hpp; any disagreement on any input is a bug in
// one of them. On top of that, every input is checked for:
//
//   1. every reported frame really has a valid checksum and a legal LEN,
//   2. re-encoding a reported frame and parsing it again gives the same frame,
//   3. after any input, the parser recovers: once the idle gap is signalled with
//      abandon() the next frame parses, and even without it a short run of valid frames
//      gets through.
//
// Builds two ways: with -DMUZZLE_LIBFUZZER as a libFuzzer target (clang), or as a plain
// executable that runs a seeded random campaign (any compiler, used by ctest).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "muzzle/feetech.hpp"
#include "muzzle/frame_parser.hpp"

using namespace muzzle::feetech;
using Event = FrameParser::Event;

namespace {

struct Frame {
    std::uint8_t id, len, code, chk;
    std::vector<std::uint8_t> params;
    bool operator==(const Frame& o) const {
        return id == o.id && len == o.len && code == o.code && chk == o.chk && params == o.params;
    }
};

struct Result {
    std::vector<Frame> frames;
    std::size_t bad_checksum = 0;
    std::size_t bad_length = 0;
};

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "PROPERTY VIOLATED: %s\n", what);
    std::abort();
}

// Reference parser: scans the whole buffer by index.
Result reference_parse(const std::uint8_t* d, std::size_t n) {
    Result r;
    std::size_t i = 0;
    while (i < n) {
        if (d[i] != 0xFF) { ++i; continue; }
        if (i + 1 >= n) break;
        if (d[i + 1] != 0xFF) { i += 2; continue; }
        std::size_t j = i + 2;
        while (j < n && d[j] == 0xFF) ++j;  // extra FFs shift the header
        if (j + 1 >= n) break;              // no LEN byte yet
        const std::uint8_t id = d[j];
        const std::uint8_t len = d[j + 1];
        if (len < 2 || len > 250) {
            ++r.bad_length;
            i = (len == 0xFF) ? j + 1 : j + 2;
            continue;
        }
        const std::size_t chk_at = j + 1 + len;  // code at j+2, params up to j+len
        if (chk_at >= n) break;                  // incomplete frame at the end
        std::uint32_t sum = id;
        for (std::size_t k = j + 1; k < chk_at; ++k) sum += d[k];
        Frame f{id, len, d[j + 2], d[chk_at],
                std::vector<std::uint8_t>(d + j + 3, d + chk_at)};
        if (static_cast<std::uint8_t>(~sum) == f.chk) r.frames.push_back(std::move(f));
        else ++r.bad_checksum;
        i = chk_at + 1;
    }
    return r;
}

Result streaming_parse(FrameParser& p, const std::uint8_t* d, std::size_t n) {
    Result r;
    for (std::size_t i = 0; i < n; ++i) {
        switch (p.feed(d[i])) {
            case Event::kFrame: {
                const FrameView& v = p.frame();
                r.frames.push_back({v.id, v.len, v.code, v.chk,
                                    std::vector<std::uint8_t>(v.params, v.params + v.param_count)});
                break;
            }
            case Event::kBadChecksum: ++r.bad_checksum; break;
            case Event::kBadLength: ++r.bad_length; break;
            case Event::kNone: break;
        }
    }
    return r;
}

// A frame whose body holds no 0xFF, so a scan that lands anywhere inside a run of them
// can only lock onto a real header. Checksum is 0xBC.
const std::uint8_t kProbe[] = {0xFF, 0xFF, 0x03, 0x04, 0x02, 0x38, 0x02, 0xBC};

void check_one(const std::uint8_t* data, std::size_t size) {
    FrameParser p;
    const Result s = streaming_parse(p, data, size);
    const Result ref = reference_parse(data, size);

    if (s.frames.size() != ref.frames.size()) fail("frame count differs from reference");
    for (std::size_t i = 0; i < s.frames.size(); ++i)
        if (!(s.frames[i] == ref.frames[i])) fail("frame content differs from reference");
    if (s.bad_checksum != ref.bad_checksum) fail("bad-checksum count differs from reference");
    if (s.bad_length != ref.bad_length) fail("bad-length count differs from reference");
    if (p.stats().frames != s.frames.size()) fail("stats.frames disagrees with events");

    for (const Frame& f : s.frames) {
        if (f.len < kMinLen || f.len > kMaxLen) fail("frame with illegal LEN reported");
        if (f.params.size() != static_cast<std::size_t>(f.len) - 2) fail("param count != LEN-2");
        if (f.id == 0xFF) fail("frame with ID 0xFF reported");
        if (checksum(f.id, f.len, f.code, f.params.data(), f.params.size()) != f.chk)
            fail("frame with invalid checksum reported");

        std::uint8_t buf[kMaxFrameBytes];
        const std::size_t n = encode_frame(buf, sizeof buf, f.id, f.code, f.params.data(), f.params.size());
        if (n != f.params.size() + 6) fail("re-encode failed");
        FrameParser q;
        const Result again = streaming_parse(q, buf, n);
        if (again.frames.size() != 1 || !(again.frames[0] == f)) fail("encode/parse round trip changed the frame");
    }

    // Recovery with the idle-gap signal: the very next frame must parse.
    {
        FrameParser r = p;
        r.abandon();
        const Result after = streaming_parse(r, kProbe, sizeof kProbe);
        if (after.frames.size() != 1) fail("frame after abandon() was not parsed");
    }
    // Recovery without it: a partial frame can swallow at most 253 bytes, so 40 probes
    // (320 bytes) must let at least one through.
    {
        FrameParser r = p;
        std::size_t got = 0;
        for (int k = 0; k < 40; ++k) got += streaming_parse(r, kProbe, sizeof kProbe).frames.size();
        if (got == 0) fail("parser never resynchronised on a run of valid frames");
    }
}

}  // namespace

#if defined(MUZZLE_LIBFUZZER)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    check_one(data, size);
    return 0;
}

#else

namespace {

// Small deterministic PRNG so failures reproduce from the printed seed.
struct Rng {
    std::uint64_t s;
    std::uint32_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 11);
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

// Inputs that stress the interesting paths: valid frames, corrupted frames, FF runs,
// truncations, impossible LENs, and raw noise, spliced together.
std::vector<std::uint8_t> generate(Rng& rng) {
    std::vector<std::uint8_t> out;
    const std::uint32_t pieces = 1 + rng.below(12);
    for (std::uint32_t p = 0; p < pieces; ++p) {
        switch (rng.below(6)) {
            case 0: {  // valid frame
                std::uint8_t params[kMaxParams];
                const std::size_t n = rng.below(4) == 0 ? rng.below(kMaxParams + 1) : rng.below(24);
                for (std::size_t i = 0; i < n; ++i) params[i] = static_cast<std::uint8_t>(rng.next());
                std::uint8_t buf[kMaxFrameBytes];
                const std::size_t len = encode_frame(buf, sizeof buf, static_cast<std::uint8_t>(rng.below(0xFF)),
                                                     static_cast<std::uint8_t>(rng.next()), params, n);
                out.insert(out.end(), buf, buf + len);
                break;
            }
            case 1: {  // valid frame with one flipped bit
                std::uint8_t params[16];
                const std::size_t n = rng.below(16);
                for (std::size_t i = 0; i < n; ++i) params[i] = static_cast<std::uint8_t>(rng.next());
                std::uint8_t buf[32];
                const std::size_t len = encode_frame(buf, sizeof buf, static_cast<std::uint8_t>(rng.below(0xFE)),
                                                     inst::kWrite, params, n);
                buf[rng.below(static_cast<std::uint32_t>(len))] ^= static_cast<std::uint8_t>(1u << rng.below(8));
                out.insert(out.end(), buf, buf + len);
                break;
            }
            case 2: out.insert(out.end(), 1 + rng.below(5), 0xFF); break;
            case 3: {  // header then an impossible LEN
                const std::uint8_t lens[] = {0, 1, 251, 0xFE, 0xFF};
                out.push_back(0xFF); out.push_back(0xFF);
                out.push_back(static_cast<std::uint8_t>(rng.below(0xFF)));
                out.push_back(lens[rng.below(5)]);
                break;
            }
            case 4: {  // truncated frame
                std::uint8_t params[8] = {1, 2, 3, 4, 5, 6, 7, 8};
                std::uint8_t buf[16];
                const std::size_t len = encode_frame(buf, sizeof buf, 1, inst::kWrite, params, 8);
                out.insert(out.end(), buf, buf + rng.below(static_cast<std::uint32_t>(len)));
                break;
            }
            default: {  // noise
                const std::uint32_t n = rng.below(20);
                for (std::uint32_t i = 0; i < n; ++i) out.push_back(static_cast<std::uint8_t>(rng.next()));
                break;
            }
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 20000;
    const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0x5EED;
    Rng rng{seed * 2654435761u + 1};
    std::size_t bytes = 0;
    for (long i = 0; i < iterations; ++i) {
        const auto input = generate(rng);
        bytes += input.size();
        check_one(input.data(), input.size());
    }
    std::printf("%ld inputs, %zu bytes, all properties held (seed %llu)\n", iterations, bytes,
                static_cast<unsigned long long>(seed));
    return 0;
}

#endif
