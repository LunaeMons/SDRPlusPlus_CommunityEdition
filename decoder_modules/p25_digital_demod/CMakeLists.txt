cmake_minimum_required(VERSION 3.13)
project(p25_digital_demod)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(p25_digital_demod PRIVATE "src/")
target_include_directories(p25_digital_demod PRIVATE "../digital_demod_base/src/")

# Link against the base digital demodulator library
target_link_libraries(p25_digital_demod PRIVATE digital_demod_base)
