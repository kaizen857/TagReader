cmake_minimum_required(VERSION 3.10)

# 1. 项目信息
project(TagReaderProject VERSION 1.0 LANGUAGES CXX)

# 指定 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. 包含头文件目录
# 使用 target_include_directories 而不是 include_directories 是更好的实践
# 这样可以确保头文件路径与特定的 target 绑定
include_directories(include)

# 3. 查找源文件
# 明确列出文件比使用 file(GLOB ...) 更稳健，能避免新增文件时 CMake 没感知的问题
set(SOURCES 
    src/TagReader.cpp
)

# 4. 生成可执行文件
# 假设你的 TagReader.cpp 里有 main 函数，或者你还有其他包含 main 的文件
add_executable(TagReaderApp ${SOURCES}
    include/Tag.hpp
    include/Lyric.hpp)

# 如果你想把它编译成一个库供其他程序使用：
# add_library(TagReaderLib STATIC ${SOURCES})

# 5. (可选) 如果你使用了 target 模式，建议这样指定头文件路径：
# target_include_directories(TagReaderApp PRIVATE include)