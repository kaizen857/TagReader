#ifndef TAGREADER_FORMATS_CUE_CUEPARSER_HPP
#define TAGREADER_FORMATS_CUE_CUEPARSER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tagreader_cue
{
constexpr std::size_t kMaxCueLines = 10000;
constexpr std::size_t kMaxCueTracks = 99;
constexpr std::size_t kMaxCueFileRefs = 256;
constexpr std::size_t kMaxCueIndexesPerTrack = 99;
constexpr std::size_t kMaxCueFieldBytes = 64ULL * 1024ULL;

struct CueIndex
{
    std::uint8_t number{};
    std::uint16_t minute{};
    std::uint8_t second{};
    std::uint8_t frame{};
};

struct CueTrack
{
    std::uint8_t number{};
    std::string type;
    std::string title;
    std::string performer;
    std::string songwriter;
    std::vector<CueIndex> indexes;
};

struct CueFile
{
    std::string name;
    std::string format;
    std::string title;
    std::string performer;
    std::string songwriter;
    std::vector<CueTrack> tracks;
};

struct CueGlobal
{
    std::string title;
    std::string performer;
    std::string songwriter;
    std::string genre;
    std::string date;
    std::string year;
    std::string discNumber;
};

struct ParsedCueSheet
{
    CueGlobal global;
    std::vector<CueFile> files;
};

std::optional<ParsedCueSheet> ParseCueSheet(std::string_view cueText);
}

#endif
