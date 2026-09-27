// ESP32-C3 bench for the muzzle protocol layer.
//
// Two questions, answered on the chip itself and printed over USB serial every few seconds:
//
//   1. How many CPU cycles does the parser spend per byte? Timed with the core's cycle
//      counter, with interrupts off (pure compute) and on (what a real loop sees).
//   2. How long does a byte take to get from UART1's TX pin back to readable data on its
//      RX pin, with the Arduino driver's defaults versus tuned settings? This is where
//      "the ESP32 takes 100 ms" style delays come from, so it is measured, not assumed.
//      Runs twice: once with the UART's internal loopback (TX tied to RX inside the chip,
//      no wire needed), and once through the pins, which needs a jumper GPIO4 -> GPIO5
//      and is skipped cleanly if the wire is not there.
#include <Arduino.h>

#include <cstring>

#include "driver/uart.h"
#include "hal/cpu_ll.h"
#include "hal/uart_ll.h"
#include "soc/uart_struct.h"
#include "muzzle/frame_parser.hpp"
#include "muzzle/sample_traffic.hpp"

namespace {

using muzzle::feetech::FrameParser;
using Event = FrameParser::Event;

constexpr int kTxPin = 4;
constexpr int kRxPin = 5;
constexpr uint32_t kBaud = 1000000;
constexpr uint32_t kSteps = 16;
constexpr int kLoopTrials = 200;

uint8_t g_traffic[muzzle::sample::kMaxStepBytes * kSteps];
size_t g_traffic_len = 0;
uint32_t g_cpu_mhz = 0;

struct Stat {
    uint32_t min = UINT32_MAX;
    uint32_t max = 0;
    uint64_t sum = 0;
    uint32_t n = 0;
    void add(uint32_t v) {
        if (v < min) min = v;
        if (v > max) max = v;
        sum += v;
        ++n;
    }
    double mean() const { return n ? static_cast<double>(sum) / n : 0.0; }
};

inline uint32_t cycles() { return cpu_ll_get_cycle_count(); }

// Cost of reading the counter twice with nothing in between; subtracted from every sample.
uint32_t counter_overhead() {
    uint32_t best = UINT32_MAX;
    for (int i = 0; i < 1000; ++i) {
        const uint32_t a = cycles();
        const uint32_t b = cycles();
        if (b - a < best) best = b - a;
    }
    return best;
}

struct ParserResult {
    Stat per_byte;          // cycles, per feed() call
    uint32_t pass_cycles;   // one whole pass without per-byte timing
    uint32_t frames;
};

ParserResult bench_parser(bool interrupts_off) {
    ParserResult r{};
    FrameParser p;
    const uint32_t oh = counter_overhead();

    if (interrupts_off) noInterrupts();
    for (int pass = 0; pass < 4; ++pass) {
        for (size_t i = 0; i < g_traffic_len; ++i) {
            const uint32_t t0 = cycles();
            const Event e = p.feed(g_traffic[i]);
            const uint32_t t1 = cycles();
            r.per_byte.add(t1 - t0 > oh ? t1 - t0 - oh : 0);
            r.frames += e == Event::kFrame;
        }
    }
    p.reset();
    const uint32_t t0 = cycles();
    for (size_t i = 0; i < g_traffic_len; ++i) p.feed(g_traffic[i]);
    r.pass_cycles = cycles() - t0;
    if (interrupts_off) interrupts();
    return r;
}

struct LoopResult {
    bool wired = false;
    Stat byte_us;   // write one byte -> readable on RX
    Stat frame_us;  // write one 26-byte goal frame -> parser reports it
};

// Spin until UART1 has data or `limit_us` passes. Returns elapsed microseconds, or -1.
int32_t wait_readable(uint32_t t0, uint32_t limit_us) {
    const uint32_t limit = limit_us * g_cpu_mhz;
    while (!Serial1.available()) {
        if (cycles() - t0 > limit) return -1;
    }
    return static_cast<int32_t>((cycles() - t0) / g_cpu_mhz);
}

LoopResult bench_loopback(bool tuned, bool internal) {
    LoopResult r;
    Serial1.end();
    // Arduino's default: RX FIFO interrupt after 112 bytes, else the driver's idle timeout.
    Serial1.begin(kBaud, SERIAL_8N1, kRxPin, kTxPin, false, 20000UL, tuned ? 1 : 112);
    if (tuned) Serial1.setRxTimeout(1);
    // begin() reconfigures the port, so the loopback bit is set (or cleared) after it.
    uart_set_loop_back(UART_NUM_1, internal);
    delay(20);
    while (Serial1.available()) Serial1.read();

    for (int i = 0; i < kLoopTrials; ++i) {
        const uint32_t t0 = cycles();
        Serial1.write(static_cast<uint8_t>(0x55));
        const int32_t us = wait_readable(t0, 200000);
        if (us < 0) return r;  // nothing came back: no wire
        r.byte_us.add(static_cast<uint32_t>(us));
        while (Serial1.available()) Serial1.read();
    }
    r.wired = true;

    // One LeRobot-shaped goal frame, parsed as it comes back.
    uint8_t frame[32];
    uint8_t goals[12];
    for (int i = 0; i < 6; ++i)
        muzzle::feetech::write_u16_le(goals + 2 * i, static_cast<uint16_t>(2000 + i));
    const size_t n = muzzle::feetech::encode_sync_write(frame, sizeof frame, muzzle::feetech::reg::kGoalPosition,
                                                        2, muzzle::sample::kArmIds, 6, goals);
    FrameParser p;
    for (int i = 0; i < kLoopTrials; ++i) {
        p.abandon();
        const uint32_t t0 = cycles();
        Serial1.write(frame, n);
        bool done = false;
        while (!done) {
            if (cycles() - t0 > 200000u * g_cpu_mhz) break;
            while (Serial1.available() && !done) done = p.feed(static_cast<uint8_t>(Serial1.read())) == Event::kFrame;
        }
        if (!done) break;
        r.frame_us.add((cycles() - t0) / g_cpu_mhz);
    }
    return r;
}

// Same loopback, but below the driver: the driver's RX interrupt is switched off and the
// hardware FIFO is polled and read straight from the UART registers. This is the path a
// real in-line device would use, so it shows what the silicon costs without the driver.
struct DirectResult {
    bool ok = false;
    Stat byte_cycles;
    Stat frame_cycles;
};

DirectResult bench_direct() {
    DirectResult r;
    Serial1.end();
    Serial1.begin(kBaud, SERIAL_8N1, kRxPin, kTxPin, false, 20000UL, 1);
    uart_set_loop_back(UART_NUM_1, true);
    uart_disable_rx_intr(UART_NUM_1);
    delay(5);
    uart_ll_rxfifo_rst(&UART1);

    const uint32_t limit = 200000u * g_cpu_mhz;
    for (int i = 0; i < kLoopTrials; ++i) {
        const uint8_t b = 0x55;
        const uint32_t t0 = cycles();
        uart_ll_write_txfifo(&UART1, &b, 1);
        while (uart_ll_get_rxfifo_len(&UART1) == 0) {
            if (cycles() - t0 > limit) return r;
        }
        r.byte_cycles.add(cycles() - t0);
        uint8_t sink;
        uart_ll_read_rxfifo(&UART1, &sink, 1);
    }

    uint8_t frame[32];
    uint8_t goals[12];
    for (int i = 0; i < 6; ++i)
        muzzle::feetech::write_u16_le(goals + 2 * i, static_cast<uint16_t>(2000 + i));
    const size_t n = muzzle::feetech::encode_sync_write(frame, sizeof frame, muzzle::feetech::reg::kGoalPosition,
                                                        2, muzzle::sample::kArmIds, 6, goals);
    FrameParser p;
    for (int i = 0; i < kLoopTrials; ++i) {
        p.abandon();
        const uint32_t t0 = cycles();
        uart_ll_write_txfifo(&UART1, frame, static_cast<uint32_t>(n));
        bool done = false;
        while (!done) {
            if (cycles() - t0 > limit) return r;
            uint32_t avail = uart_ll_get_rxfifo_len(&UART1);
            while (avail-- && !done) {
                uint8_t byte;
                uart_ll_read_rxfifo(&UART1, &byte, 1);
                done = p.feed(byte) == Event::kFrame;
            }
        }
        r.frame_cycles.add(cycles() - t0);
    }
    r.ok = true;
    return r;
}

void print_direct(const DirectResult& r) {
    if (!r.ok) {
        Serial.println("uart loopback, internal, direct FIFO: nothing came back");
        return;
    }
    const double mhz = g_cpu_mhz;
    Serial.printf("uart loopback, internal, direct FIFO (no driver): 1 byte min/mean/max = %.1f / %.1f / %.1f us "
                  "(wire time 10 us); 26-byte frame min/mean/max = %.1f / %.1f / %.1f us (wire time 260 us)\n",
                  r.byte_cycles.min / mhz, r.byte_cycles.mean() / mhz, r.byte_cycles.max / mhz,
                  r.frame_cycles.min / mhz, r.frame_cycles.mean() / mhz, r.frame_cycles.max / mhz);
}

void print_parser(const char* label, const ParserResult& r) {
    const double per_byte_pass = static_cast<double>(r.pass_cycles) / static_cast<double>(g_traffic_len);
    Serial.printf("parser, %s: per byte min/mean/max = %u / %.1f / %u cycles (max %.2f us); "
                  "whole pass %u bytes in %u cycles = %.1f cycles/byte; frames %u\n",
                  label, r.per_byte.min, r.per_byte.mean(), r.per_byte.max,
                  static_cast<double>(r.per_byte.max) / g_cpu_mhz, static_cast<unsigned>(g_traffic_len),
                  r.pass_cycles, per_byte_pass, r.frames);
}

void print_loop(const char* label, const LoopResult& r) {
    if (!r.wired) {
        if (std::strstr(label, "internal")) {
            Serial.printf("uart loopback, %s: nothing came back, internal loopback failed\n", label);
            return;
        }
        Serial.printf("uart loopback, %s: no loopback wire (GPIO%d -> GPIO%d), skipped\n", label, kTxPin, kRxPin);
        return;
    }
    Serial.printf("uart loopback, %s: 1 byte min/mean/max = %u / %.1f / %u us (wire time 10 us); "
                  "26-byte frame min/mean/max = %u / %.1f / %u us (wire time 260 us)\n",
                  label, r.byte_us.min, r.byte_us.mean(), r.byte_us.max,
                  r.frame_us.min, r.frame_us.mean(), r.frame_us.max);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t start = millis();
    while (!Serial && millis() - start < 3000) delay(10);

    cpu_ll_enable_cycle_count();
    g_cpu_mhz = getCpuFrequencyMhz();

    for (uint32_t s = 0; s < kSteps; ++s)
        g_traffic_len += muzzle::sample::append_control_step(g_traffic + g_traffic_len,
                                                             sizeof g_traffic - g_traffic_len, s);
}

void loop() {
    Serial.println();
    Serial.println("muzzle c3 bench");

    // Sanity check the counter against the microsecond timer before trusting it.
    const uint32_t c0 = cycles();
    delayMicroseconds(1000);
    const uint32_t check = cycles() - c0;
    Serial.printf("cpu %u MHz; counter check: 1000 us = %u cycles (expect about %u)\n",
                  g_cpu_mhz, check, g_cpu_mhz * 1000);

    print_parser("interrupts off", bench_parser(true));
    print_parser("interrupts on", bench_parser(false));
    print_loop("internal, Arduino defaults", bench_loopback(false, true));
    print_loop("internal, tuned: fifo 1 byte, timeout 1 symbol", bench_loopback(true, true));
    print_direct(bench_direct());
    print_loop("pins, Arduino defaults", bench_loopback(false, false));
    print_loop("pins, tuned: fifo 1 byte, timeout 1 symbol", bench_loopback(true, false));

    delay(5000);
}
