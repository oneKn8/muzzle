// Parser throughput on the laptop, on the same synthetic control-step bytes the ESP32-C3
// bench uses. Only a reference point: the number that matters is cycles per byte on the
// microcontroller, measured by firmware/c3_bench.
#include <chrono>
#include <cstdio>
#include <vector>

#include "muzzle/frame_parser.hpp"
#include "muzzle/sample_traffic.hpp"

int main() {
    using namespace muzzle;
    constexpr std::uint32_t kSteps = 64;
    std::vector<std::uint8_t> traffic(sample::kMaxStepBytes * kSteps);
    std::size_t n = 0;
    for (std::uint32_t s = 0; s < kSteps; ++s)
        n += sample::append_control_step(traffic.data() + n, traffic.size() - n, s);
    traffic.resize(n);

    feetech::FrameParser parser;
    constexpr int kPasses = 20000;
    std::uint64_t frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int pass = 0; pass < kPasses; ++pass)
        for (std::uint8_t b : traffic)
            frames += parser.feed(b) == feetech::FrameParser::Event::kFrame;
    const auto t1 = std::chrono::steady_clock::now();

    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double total_bytes = static_cast<double>(n) * kPasses;
    std::printf("%zu bytes x %d passes, %llu frames\n", n, kPasses, static_cast<unsigned long long>(frames));
    std::printf("%.2f ns per byte (wire time at 1 Mbps is 10000 ns per byte)\n", ns / total_bytes);
    return frames == static_cast<std::uint64_t>(kSteps) * sample::kFramesPerStep * kPasses ? 0 : 1;
}
