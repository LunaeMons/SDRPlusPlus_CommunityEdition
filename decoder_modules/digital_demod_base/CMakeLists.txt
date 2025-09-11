cmake_minimum_required(VERSION 3.13)
project(digital_demod_base)

set(SRC
    "src/digital_demod_base.cpp"
    "src/digital_network_sink.cpp" 
    "src/digital_file_sink.cpp"
)

include(${SDRPP_MODULE_CMAKE})

target_include_directories(digital_demod_base PRIVATE "src/")

# This is a base library, not a standalone module
# Other digital demodulator modules will link against this
set_target_properties(digital_demod_base PROPERTIES 
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
