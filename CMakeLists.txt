cmake_minimum_required(VERSION 3.10)

# 1. 项目信息
project(TagReaderProject VERSION 1.0 LANGUAGES CXX)

# 指定 C++ 标准
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TAGREADER_ENABLE_SANITIZERS "Enable ASAN/UBSAN" OFF)
option(TAGREADER_ENABLE_FUZZING "Enable fuzz targets" OFF)
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
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# 3. 库目标
add_library(TagReaderCore STATIC
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
)

tagreader_enable_sanitizers(TagReaderCore)

target_include_directories(TagReaderCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
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
