cmake_minimum_required(VERSION 3.10)

# 1. 项目信息
project(TagReaderProject VERSION 1.0 LANGUAGES CXX)

# 指定 C++ 标准
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. 依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

# 3. 库目标
add_library(TagReaderCore STATIC
    src/TagReader.cpp
)

target_include_directories(TagReaderCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(TagReaderCore
    PUBLIC
        PkgConfig::FFMPEG
)

# 4. 轻量级测试程序
add_executable(TagReaderTest
    test/main.cpp
)

target_include_directories(TagReaderTest
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(TagReaderTest
    PRIVATE
        PkgConfig::FFMPEG
        TagReaderCore
)

# 5. 如果需要默认构建测试程序
set_target_properties(TagReaderTest PROPERTIES OUTPUT_NAME "TagReaderTest")
