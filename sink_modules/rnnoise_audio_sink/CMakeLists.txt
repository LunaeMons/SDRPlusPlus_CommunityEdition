cmake_minimum_required(VERSION 3.13)
project(rnnoise_audio_sink)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

# Find RNNoise first (common for all platforms)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(RNNOISE rnnoise)
endif()

if (MSVC)
    # Windows specific settings
    # RtAudio
    target_link_directories(rnnoise_audio_sink PRIVATE "C:/Program Files (x86)/RtAudio/lib")
    target_include_directories(rnnoise_audio_sink PRIVATE "C:/Program Files (x86)/RtAudio/include/rtaudio")
    target_link_libraries(rnnoise_audio_sink PRIVATE rtaudio)
    
    # RNNoise for Windows
    # You'll need to provide the path to RNNoise library on Windows
    # Example paths (adjust according to your installation):
    target_include_directories(rnnoise_audio_sink PRIVATE "C:/rnnoise/include")
    target_link_directories(rnnoise_audio_sink PRIVATE "C:/rnnoise/lib")
    target_link_libraries(rnnoise_audio_sink PRIVATE rnnoise)
    
else (MSVC)
    # Linux/Unix settings
    
    # RtAudio
    pkg_check_modules(RTAUDIO REQUIRED rtaudio)
    target_include_directories(rnnoise_audio_sink PRIVATE ${RTAUDIO_INCLUDE_DIRS})
    target_link_directories(rnnoise_audio_sink PRIVATE ${RTAUDIO_LIBRARY_DIRS})
    target_link_libraries(rnnoise_audio_sink PRIVATE ${RTAUDIO_LIBRARIES})

    # RNNoise
    if(RNNOISE_FOUND)
        # Use pkg-config if found
        target_include_directories(rnnoise_audio_sink PRIVATE ${RNNOISE_INCLUDE_DIRS})
        target_link_directories(rnnoise_audio_sink PRIVATE ${RNNOISE_LIBRARY_DIRS})
        target_link_libraries(rnnoise_audio_sink PRIVATE ${RNNOISE_LIBRARIES})
    else()
        # Manual search for RNNoise
        find_path(RNNOISE_INCLUDE_DIR 
            NAMES rnnoise.h
            PATHS /usr/local/include /usr/include
        )
        
        find_library(RNNOISE_LIBRARY
            NAMES rnnoise
            PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu
        )
        
        if(RNNOISE_INCLUDE_DIR AND RNNOISE_LIBRARY)
            target_include_directories(rnnoise_audio_sink PRIVATE ${RNNOISE_INCLUDE_DIR})
            target_link_libraries(rnnoise_audio_sink PRIVATE ${RNNOISE_LIBRARY})
            message(STATUS "Found RNNoise: ${RNNOISE_LIBRARY}")
        else()
            message(FATAL_ERROR "RNNoise not found! Please install librnnoise-dev or build from source")
        endif()
    endif()
    
    # Add common library paths
    target_link_directories(rnnoise_audio_sink PRIVATE /usr/local/lib /usr/lib/x86_64-linux-gnu)
    
    # Set RPATH for runtime library location
    set_target_properties(rnnoise_audio_sink PROPERTIES
        INSTALL_RPATH "/usr/local/lib:/usr/lib:/usr/lib/x86_64-linux-gnu"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif ()

# Compiler flags - no need to duplicate what's in SDRPP_MODULE_CMAKE
# The -O3 and -std=c++17 are already set by SDRPP_MODULE_COMPILER_FLAGS

# Remove prefix for module name
set_target_properties(rnnoise_audio_sink PROPERTIES PREFIX "")

# Install target
install(TARGETS rnnoise_audio_sink DESTINATION lib/sdrpp/plugins)