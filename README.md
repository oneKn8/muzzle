# muzzle

The protocol layer for **muzzle**, a small in-line device for the Feetech STS servo bus
that LeRobot's SO-101 arm runs on. muzzle sits on the wire between the computer and the
servos, so it has to understand every byte at bus speed on a few-dollar microcontroller (ESP32-C3).
This repo is the foundation that makes that possible: a streaming parser and encoder for
Feetech frames, written in C++17 with no heap, no exceptions and no RTTI, plus the tests,
fuzzing and on-chip benchmarks that show it holds up.

Part of [imp](https://github.com/oneKn8/imp).

## What is in here

| Path | What it is |
|---|---|
| `include/muzzle/feetech.hpp` | Frame layout, checksum, instruction codes, the registers this project uses, frame builders |
| `include/muzzle/frame_parser.hpp` | Streaming parser: one byte per call, constant work per byte, never backtracks or allocates |
| `tests/` | Unit tests built on the official example frames from Feetech's protocol manual |
| `fuzz/` | Differential fuzzer: the streaming parser against an independent whole-buffer parser |
| `bench/host_bench.cpp` | Parser throughput on the laptop |
| `firmware/c3_bench/` | ESP32-C3 firmware: cycles per byte on the chip, and real UART loopback latency |
| `docs/feetech-protocol.md` | The protocol, every claim cited to Feetech's manuals, SDKs, or the LeRobot driver |

## Design notes

**Why byte-at-a-time.** At 1 Mbps a byte lands every 10 us. Anything that makes decisions
about a frame has to keep pace while the frame is still arriving, so `feed()` does a fixed
amount of work per call and returns an event the moment a frame completes.

**Resynchronisation rules.** This is where hand-rolled serial parsers usually break.
Extra `FF` bytes shift the header; an impossible LEN (below 2, above 250) is rejected and,
if it was itself `FF`, treated as the start of the next header; a bad checksum drops the
frame and scanning restarts at the next byte. A frame cut off mid-way would swallow the
next frame's bytes, so the owner calls `abandon()` when the UART reports the line went
idle mid-frame. The rules are pinned by unit tests and checked against a second,
independently written parser by the fuzzer.

**Same code on both machines.** The ESP32-C3 toolchain in PlatformIO is GCC 8.4, so the
core stays C++17. The per-byte function is placed in instruction RAM on the chip
(`MUZZLE_HOT`), so a flash-cache miss can never land in the middle of a byte.

## Results so far

Measured on the laptop (x86-64, GCC 11.4, clang 14):

| Check | Result |
|---|---|
| Unit tests (manual's example frames, resync cases, limits) | 93 checks, 0 failures, under ASan + UBSan |
| Seeded differential campaign | 1,000,000 inputs, 88.8 MB, every property held |
| libFuzzer, ASan + UBSan, 3 minutes | 1.37 million runs, no failures, coverage plateaued at 185 edges |
| Parser throughput, Release | 4.66 ns per byte (a byte takes 10,000 ns on the wire) |
| ESP32-C3 build | compiles; `FrameParser::feed` is 348 bytes, placed in IRAM at `0x4038025c` |

Measured on an ESP32-C3 (rev v0.4, 160 MHz, cycle counter checked against the
microsecond timer: 1000 us = ~160,100 cycles), on 1,408 bytes of LeRobot-shaped traffic:

| Check | Result |
|---|---|
| Parser per byte, interrupts off | min 26 / mean 31.8 / max 83 cycles (worst 0.52 us) |
| Whole pass, no per-byte timing | 38.0 cycles per byte = 0.24 us, 2.4% of a byte's 10 us wire time |
| Parser per byte, interrupts on | same mean; an interrupt landing mid-byte pushed one sample to ~1,020 cycles (6.4 us) |

The parser is not the bottleneck. Interrupt jitter is: up to 6.4 us at random moments,
which is why the byte path should run inside the UART interrupt rather than the main loop.

Not measured yet: UART loopback latency (needs the GPIO4 -> GPIO5 jumper).

## Running it

```sh
make test       # GCC + sanitizers: unit tests, then a 1M-input differential campaign
make fuzz       # clang libFuzzer for FUZZ_SECONDS (default 180)
make bench      # host throughput
make firmware   # cross-compile the ESP32-C3 bench
```

On the board: `cd firmware/c3_bench && pio run -t upload && pio device monitor`. For the
UART part, add one jumper wire from GPIO4 to GPIO5.

## Status

Protocol layer: working and tested, on the laptop and on the chip. UART latency: pending. Everything
built on top of this lives in later work.
