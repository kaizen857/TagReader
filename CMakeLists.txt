cmake_minimum_required(VERSION 3.10)

# 1. 项目信息
project(TagReaderProject VERSION 1.0 LANGUAGES CXX)

# 指定 C++ 标准
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 多线程编译配置
include(ProcessorCount)
ProcessorCount(N)
if(NOT N EQUAL 0)
    set(CMAKE_BUILD_PARALLEL_LEVEL ${N} CACHE STRING "Number of parallel build jobs" FORCE)
    message(STATUS "Detected ${N} CPU cores, enabling parallel build with ${N} jobs")
    
    # MSVC 特定：设置 /MP 标志以启用多核编译
    if(MSVC)
        add_compile_options(/MP${N})
        message(STATUS "MSVC: Enabled multi-processor compilation with /MP${N}")
    endif()
else()
    message(WARNING "Could not detect CPU count, parallel build may not be optimal")
endif()

# Release 模式优化配置
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-O3 -march=native -DNDEBUG)
    elseif(MSVC)
        add_compile_options(/O2 /GL /DNDEBUG)
        add_link_options(/LTCG)
    endif()
    # Release 模式下强制禁用性能分析
    set(TAGREADER_ENABLE_PROFILING OFF CACHE BOOL "Profiling is disabled in Release mode" FORCE)
endif()

# Profile 模式优化配置 - 使用与 Release 相同的优化，但保留调试符号
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" AND TAGREADER_ENABLE_PROFILING)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # 使用与 Release 相同的优化级别，确保性能分析结果准确
        add_compile_options(-O3 -march=native -DNDEBUG -g)
        message(STATUS "Profile build: Using -O3 -march=native (same as Release)")
    elseif(MSVC)
        add_compile_options(/O2 /DNDEBUG /Zi)
        message(STATUS "Profile build: Using /O2 (same as Release)")
    endif()
endif()

option(TAGREADER_ENABLE_SANITIZERS "Enable ASAN/UBSAN" OFF)
option(TAGREADER_ENABLE_FUZZING "Enable fuzz targets" OFF)
option(TAGREADER_ENABLE_PROFILING "Enable Tracy performance profiling" OFF)
option(TAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS "Force-disable Iconv for explicit no-iconv policy tests" OFF)
option(TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV "Allow Latin-1 fallback when building without Iconv" OFF)
option(TAGREADER_USE_SYSTEM_CATCH2 "Prefer a system-installed Catch2 package" ON)

include(CTest)

set(TAGREADER_SANITIZER_COMPILE_OPTIONS
    -O1
    -g
    -fno-omit-frame-pointer
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
)

set(TAGREADER_SANITIZER_LINK_OPTIONS
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
)

function(tagreader_enable_sanitizers target_name)
    if(NOT TAGREADER_ENABLE_SANITIZERS)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target_name} PRIVATE ${TAGREADER_SANITIZER_COMPILE_OPTIONS})
        target_link_options(${target_name} PRIVATE ${TAGREADER_SANITIZER_LINK_OPTIONS})
    else()
        message(WARNING "TAGREADER_ENABLE_SANITIZERS is only configured for Clang/GNU compilers")
    endif()
endfunction()

# 2. 依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
if(NOT TAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS)
    find_package(Iconv)
endif()

if(NOT Iconv_FOUND AND NOT TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV)
    message(FATAL_ERROR "Iconv is required unless TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON is set explicitly")
endif()

include(FetchContent)

if(TAGREADER_USE_SYSTEM_CATCH2)
    find_package(Catch2 3 CONFIG QUIET)
endif()

if(NOT Catch2_FOUND)
    set(CATCH2_VERSION 3.7.1)
    FetchContent_Declare(
        Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v${CATCH2_VERSION}.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# Tracy Profiler 支持
if(TAGREADER_ENABLE_PROFILING)
    # Tracy 在 Arch Linux 上直接安装为系统库，不需要 pkg-config
    find_library(TRACY_LIBRARY NAMES TracyClient REQUIRED)
    set(TRACY_INCLUDE_DIR "/usr/include/Tracy")
    
    if(TRACY_LIBRARY)
        message(STATUS "Tracy profiling enabled")
        message(STATUS "Tracy library: ${TRACY_LIBRARY}")
        message(STATUS "Tracy include: ${TRACY_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "Tracy library not found. Install with: yay -S tracy")
    endif()
endif()

# 3. 库目标
set(TAGREADER_CORE_SOURCES
    src/TagReader.cpp
    src/core/TagPipeline.cpp
    src/media/FfmpegSession.cpp
    src/media/MediaInfoReader.cpp
    src/media/ContainerDetector.cpp
    src/io/ByteReader.cpp
    src/text/TextCodec.cpp
    src/text/TextNormalize.cpp
    src/cover/CoverCache.cpp
    src/cover/CoverDecoder.cpp
    src/cover/SidecarCover.cpp
    src/formats/id3/Id3Parser.cpp
    src/formats/id3/Id3Frames.cpp
    src/formats/vorbis/VorbisCommentParser.cpp
    src/formats/flac/FlacPicture.cpp
    src/formats/flac/FlacParser.cpp
    src/formats/common/BoundedReader.cpp
    src/formats/ogg-vorbis/OggVorbisParser.cpp
    src/formats/opus/OpusParser.cpp
    src/formats/riff/RiffParser.cpp
    src/formats/aiff/AiffParser.cpp
    src/formats/dsd/DsdParser.cpp
    src/formats/asf/AsfParser.cpp
    src/formats/matroska/MatroskaParser.cpp
    src/formats/mp4/Mp4AtomReader.cpp
    src/formats/mp4/Mp4Parser.cpp
    src/formats/ape/ApeParser.cpp
    src/formats/cue/CueReader.cpp
    src/formats/cue/CueParser.cpp
    src/formats/cue/CueTextLoader.cpp
    src/formats/cue/CuePathResolver.cpp
    src/formats/cue/CueTiming.cpp
    third_party/fpng/fpng.cpp
)

# 性能分析支持
if(TAGREADER_ENABLE_PROFILING)
    list(APPEND TAGREADER_CORE_SOURCES src/profiling/TracyClient.cpp)
endif()

add_library(TagReaderCore STATIC ${TAGREADER_CORE_SOURCES})

# fpng 需要特殊的编译标志以启用 SIMD 优化
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(third_party/fpng/fpng.cpp PROPERTIES COMPILE_FLAGS "-msse4.1 -mpclmul")
elseif(MSVC)
    set_source_files_properties(third_party/fpng/fpng.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX")
endif()

tagreader_enable_sanitizers(TagReaderCore)

target_include_directories(TagReaderCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fpng
)

target_link_libraries(TagReaderCore
    PUBLIC
        PkgConfig::FFMPEG
)

if(Iconv_FOUND)
    target_link_libraries(TagReaderCore PUBLIC Iconv::Iconv)
    target_compile_definitions(TagReaderCore PUBLIC TAGREADER_HAS_ICONV=1)
else()
    target_compile_definitions(TagReaderCore PUBLIC TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=1)
endif()

# Tracy 性能分析支持
if(TAGREADER_ENABLE_PROFILING)
    target_compile_definitions(TagReaderCore PUBLIC TAGREADER_ENABLE_PROFILING=1 TRACY_ENABLE=1)
    target_link_libraries(TagReaderCore PUBLIC ${TRACY_LIBRARY})
    if(TRACY_INCLUDE_DIR)
        target_include_directories(TagReaderCore PUBLIC ${TRACY_INCLUDE_DIR})
    endif()
    # 添加必要的系统库
    target_link_libraries(TagReaderCore PUBLIC pthread dl)
    message(STATUS "TagReaderCore: Tracy profiling enabled")
endif()

# 4. 轻量级测试程序
add_executable(TagReaderTest
    test/main.cpp
)

tagreader_enable_sanitizers(TagReaderTest)

target_link_libraries(TagReaderTest
    PRIVATE
        TagReaderCore
)

# 5. 如果需要默认构建测试程序
set_target_properties(TagReaderTest PROPERTIES OUTPUT_NAME "TagReaderTest")

if(TAGREADER_ENABLE_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(WARNING "TAGREADER_ENABLE_FUZZING requires Clang libFuzzer support; TagReaderFuzz will not be built")
    else()
        add_executable(TagReaderFuzz
            test/fuzz/tagreader_fuzz.cpp
        )

        tagreader_enable_sanitizers(TagReaderFuzz)

        target_compile_options(TagReaderFuzz PRIVATE -fsanitize=fuzzer)
        target_link_options(TagReaderFuzz PRIVATE -fsanitize=fuzzer)

        target_link_libraries(TagReaderFuzz
            PRIVATE
                TagReaderCore
        )
    endif()
endif()

add_subdirectory(test)
