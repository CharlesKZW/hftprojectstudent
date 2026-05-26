cmake_minimum_required(VERSION 3.16)
project(hftclient2026 LANGUAGES CXX)

add_executable(hftclient2026 main.cpp)

# Aggressive optimization — this is a latency-sensitive HFT client.
# -march=native lets the compiler use every instruction available on the
# host CPU (AVX2/AVX-512 on x86, NEON on ARM).
target_compile_features(hftclient2026 PRIVATE cxx_std_20)
target_compile_options(hftclient2026 PRIVATE
    -O3
    -DNDEBUG
    -fno-rtti
    -fno-exceptions
    -fvisibility=hidden
    -Wall -Wextra
)

# -march=native isn't accepted by Apple Clang on Apple Silicon (use -mcpu)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
if(HAS_MARCH_NATIVE)
    target_compile_options(hftclient2026 PRIVATE -march=native)
else()
    check_cxx_compiler_flag("-mcpu=apple-m1" HAS_MCPU_APPLE_M1)
    if(HAS_MCPU_APPLE_M1)
        target_compile_options(hftclient2026 PRIVATE -mcpu=apple-m1)
    endif()
endif()

# LTO if available
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_OK OUTPUT IPO_LOG)
if(IPO_OK)
    set_property(TARGET hftclient2026 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
