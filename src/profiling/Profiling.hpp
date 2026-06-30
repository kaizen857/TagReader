#ifndef TAGREADER_PROFILING_HPP
#define TAGREADER_PROFILING_HPP

// TagReader 性能分析宏定义
// 使用 Tracy Profiler 进行细粒度性能追踪

#ifdef TAGREADER_ENABLE_PROFILING

// Tracy 客户端头文件
#include <tracy/tracy/Tracy.hpp>

// 函数级性能追踪（自动记录整个函数）
#define TAGREADER_PROFILE_FUNCTION() ZoneScoped

// 命名区域性能追踪（用于函数内的特定代码块）
#define TAGREADER_PROFILE_SCOPE(name) ZoneScopedN(name)

// 带颜色的命名区域（用于区分不同阶段）
// 颜色值：0xFF0000 (红), 0x00FF00 (绿), 0x0000FF (蓝)
#define TAGREADER_PROFILE_SCOPE_COLOR(name, color) ZoneScopedNC(name, color)

// 为当前区域设置文本标签（用于记录额外信息）
#define TAGREADER_PROFILE_TEXT(text, size) ZoneText(text, size)

// 为当前区域设置值（用于记录数值）
#define TAGREADER_PROFILE_VALUE(value) ZoneValue(value)

// 为当前区域设置文件名
#define TAGREADER_PROFILE_NAME(name, size) ZoneName(name, size)

// 内存分配追踪
#define TAGREADER_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define TAGREADER_PROFILE_FREE(ptr) TracyFree(ptr)

// 帧标记（用于识别处理周期）
#define TAGREADER_PROFILE_FRAME_MARK() FrameMark

// 消息日志（在 Tracy 时间线中显示）
#define TAGREADER_PROFILE_MESSAGE(text, size) TracyMessage(text, size)

// 常用颜色定义
#define TAGREADER_COLOR_IO        0x4169E1  // 蓝色 - I/O 操作
#define TAGREADER_COLOR_PARSE     0xFF8C00  // 橙色 - 解析操作
#define TAGREADER_COLOR_DECODE    0x32CD32  // 绿色 - 解码操作
#define TAGREADER_COLOR_CONVERT   0x9370DB  // 紫色 - 转换操作
#define TAGREADER_COLOR_CACHE     0xFFD700  // 金色 - 缓存操作
#define TAGREADER_COLOR_DETECT    0xFF1493  // 粉色 - 检测操作
#define TAGREADER_COLOR_VALIDATE  0x00CED1  // 青色 - 验证操作
#define TAGREADER_COLOR_FFMPEG    0xDC143C  // 红色 - FFmpeg 操作

#else // TAGREADER_ENABLE_PROFILING 未定义

// 性能分析禁用时，所有宏都为空操作
#define TAGREADER_PROFILE_FUNCTION()
#define TAGREADER_PROFILE_SCOPE(name)
#define TAGREADER_PROFILE_SCOPE_COLOR(name, color)
#define TAGREADER_PROFILE_TEXT(text, size)
#define TAGREADER_PROFILE_VALUE(value)
#define TAGREADER_PROFILE_NAME(name, size)
#define TAGREADER_PROFILE_ALLOC(ptr, size)
#define TAGREADER_PROFILE_FREE(ptr)
#define TAGREADER_PROFILE_FRAME_MARK()
#define TAGREADER_PROFILE_MESSAGE(text, size)

#endif // TAGREADER_ENABLE_PROFILING

#endif // TAGREADER_PROFILING_HPP
