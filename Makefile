# Convenience targets. CMake is the real build; this just remembers the flags.
#
# CLANG_FIX works around this laptop's clang 14 picking up a GCC 12 install that has no
# libstdc++ headers. On a normal machine (and in CI) leave both empty:
#   make fuzz CLANG_INC= CLANG_LNK=
CLANG_INC ?= --gcc-toolchain=/usr -nostdinc++ -isystem /usr/include/c++/11 -isystem /usr/include/x86_64-linux-gnu/c++/11
CLANG_LNK ?= -L/usr/lib/gcc/x86_64-linux-gnu/11
FUZZ_SECONDS ?= 180

.PHONY: test fuzz bench firmware clean

test:
	cmake -S . -B build/gcc-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMUZZLE_SANITIZE=ON
	cmake --build build/gcc-san
	ctest --test-dir build/gcc-san --output-on-failure
	./build/gcc-san/muzzle_fuzz_random 1000000 7

fuzz:
	CXX=clang++ cmake -S . -B build/clang-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		-DMUZZLE_SANITIZE=ON -DMUZZLE_LIBFUZZER=ON \
		-DCMAKE_CXX_FLAGS="$(CLANG_INC)" -DCMAKE_EXE_LINKER_FLAGS="$(CLANG_LNK)"
	cmake --build build/clang-fuzz
	mkdir -p build/corpus
	./build/clang-fuzz/muzzle_fuzz build/corpus -max_total_time=$(FUZZ_SECONDS) -max_len=2048

bench:
	cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build/release
	./build/release/muzzle_host_bench

firmware:
	cd firmware/c3_bench && pio run

clean:
	rm -rf build firmware/c3_bench/.pio
