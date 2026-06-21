#include "formats/cue/CueParser.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace tagreader_cue
{
namespace
{
std::string_view TrimLeft(std::string_view text)
{
    std::size_t index = 0;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0)
    {
        ++index;
    }
    return text.substr(index);
}

std::string_view TrimRight(std::string_view text)
{
    std::size_t index = text.size();
    while (index > 0 && std::isspace(static_cast<unsigned char>(text[index - 1])) != 0)
    {
        --index;
    }
    return text.substr(0, index);
}

std::string_view Trim(std::string_view text)
{
    return TrimRight(TrimLeft(text));
}

bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < lhs.size(); ++index)
    {
        const unsigned char left = static_cast<unsigned char>(lhs[index]);
        const unsigned char right = static_cast<unsigned char>(rhs[index]);
        if (std::tolower(left) != std::tolower(right))
        {
            return false;
        }
    }

    return true;
}

std::string UpperAscii(std::string_view text)
{
    std::string upper{text};
    for (char &ch : upper)
    {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return upper;
}

bool FitsField(std::string_view text)
{
    return text.size() <= kMaxCueFieldBytes;
}

struct ParsedToken
{
    std::string_view value;
    std::string_view tail;
};

std::optional<ParsedToken> ParseToken(std::string_view text, bool allowQuoted)
{
    text = TrimLeft(text);
    if (text.empty())
    {
        return std::nullopt;
    }

    if (allowQuoted && text.front() == '"')
    {
        const std::size_t closing = text.find('"', 1);
        if (closing == std::string_view::npos)
        {
            return std::nullopt;
        }
        return ParsedToken{text.substr(1, closing - 1), text.substr(closing + 1)};
    }

    std::size_t end = 0;
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0)
    {
        ++end;
    }
    return ParsedToken{text.substr(0, end), text.substr(end)};
}

std::optional<std::uint32_t> ParseUnsigned(std::string_view text, std::uint32_t maxValue)
{
    if (text.empty())
    {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (char ch : text)
    {
        if (ch < '0' || ch > '9')
        {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
        if (value > maxValue)
        {
            return std::nullopt;
        }
    }

    return static_cast<std::uint32_t>(value);
}

std::optional<CueIndex> ParseIndexValue(std::string_view indexNumberText, std::string_view timeText)
{
    const std::optional<std::uint32_t> indexNumber = ParseUnsigned(indexNumberText, 99);
    if (!indexNumber.has_value())
    {
        return std::nullopt;
    }

    const std::size_t firstColon = timeText.find(':');
    const std::size_t secondColon = firstColon == std::string_view::npos ? std::string_view::npos : timeText.find(':', firstColon + 1);
    if (firstColon == std::string_view::npos || secondColon == std::string_view::npos)
    {
        return std::nullopt;
    }

    const std::string_view minuteText = timeText.substr(0, firstColon);
    const std::string_view secondText = timeText.substr(firstColon + 1, secondColon - firstColon - 1);
    const std::string_view frameText = timeText.substr(secondColon + 1);
    const std::optional<std::uint32_t> minute = ParseUnsigned(minuteText, std::numeric_limits<std::uint16_t>::max());
    const std::optional<std::uint32_t> second = ParseUnsigned(secondText, 59);
    const std::optional<std::uint32_t> frame = ParseUnsigned(frameText, 74);
    if (!minute.has_value() || !second.has_value() || !frame.has_value())
    {
        return std::nullopt;
    }

    return CueIndex{static_cast<std::uint8_t>(*indexNumber), static_cast<std::uint16_t>(*minute), static_cast<std::uint8_t>(*second), static_cast<std::uint8_t>(*frame)};
}

bool StoreValue(std::string &field, std::string_view value)
{
    if (!FitsField(value))
    {
        return false;
    }
    field.assign(value.begin(), value.end());
    return true;
}

CueGlobal *CurrentGlobal(ParsedCueSheet &sheet)
{
    return &sheet.global;
}
}

std::optional<ParsedCueSheet> ParseCueSheet(std::string_view cueText)
{
    ParsedCueSheet sheet;
    std::size_t lineCount = 0;
    CueFile *currentFile = nullptr;
    CueTrack *currentTrack = nullptr;

    std::size_t start = 0;
    while (start <= cueText.size())
    {
        const std::size_t end = cueText.find('\n', start);
        std::string_view line = end == std::string_view::npos ? cueText.substr(start) : cueText.substr(start, end - start);
        start = end == std::string_view::npos ? cueText.size() + 1 : end + 1;

        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        ++lineCount;
        if (lineCount > kMaxCueLines)
        {
            return std::nullopt;
        }

        line = TrimLeft(line);
        if (line.empty())
        {
            if (end == std::string_view::npos)
            {
                break;
            }
            continue;
        }

        const std::optional<ParsedToken> commandToken = ParseToken(line, false);
        if (!commandToken.has_value())
        {
            return std::nullopt;
        }

        const std::string command = UpperAscii(commandToken->value);
        std::string_view rest = TrimLeft(commandToken->tail);

        if (command == "FILE")
        {
            const std::optional<ParsedToken> nameToken = ParseToken(rest, true);
            if (!nameToken.has_value() || !ParseToken(TrimLeft(nameToken->tail), false).has_value())
            {
                return std::nullopt;
            }

            const std::optional<ParsedToken> formatToken = ParseToken(TrimLeft(nameToken->tail), false);
            if (!formatToken.has_value() || !FitsField(nameToken->value) || !FitsField(formatToken->value))
            {
                return std::nullopt;
            }

            if (sheet.files.size() >= kMaxCueFileRefs)
            {
                return std::nullopt;
            }

            sheet.files.push_back(CueFile{});
            CueFile &file = sheet.files.back();
            if (!StoreValue(file.name, nameToken->value) || !StoreValue(file.format, formatToken->value))
            {
                return std::nullopt;
            }
            currentFile = &file;
            currentTrack = nullptr;
            continue;
        }

        if (command == "TRACK")
        {
            if (currentFile == nullptr)
            {
                return std::nullopt;
            }

            const std::optional<ParsedToken> numberToken = ParseToken(rest, false);
            if (!numberToken.has_value())
            {
                return std::nullopt;
            }
            const std::optional<std::uint32_t> trackNumber = ParseUnsigned(numberToken->value, 99);
            const std::optional<ParsedToken> typeToken = ParseToken(TrimLeft(numberToken->tail), false);
            if (!trackNumber.has_value() || !typeToken.has_value() || *trackNumber == 0)
            {
                return std::nullopt;
            }

            if (currentFile->tracks.size() >= kMaxCueTracks)
            {
                return std::nullopt;
            }

            currentFile->tracks.push_back(CueTrack{});
            CueTrack &track = currentFile->tracks.back();
            track.number = static_cast<std::uint8_t>(*trackNumber);
            if (!StoreValue(track.type, typeToken->value))
            {
                return std::nullopt;
            }
            currentTrack = &track;
            continue;
        }

        if (command == "INDEX")
        {
            if (currentTrack == nullptr)
            {
                return std::nullopt;
            }

            const std::optional<ParsedToken> numberToken = ParseToken(rest, false);
            if (!numberToken.has_value())
            {
                return std::nullopt;
            }
            const std::optional<ParsedToken> timeToken = ParseToken(TrimLeft(numberToken->tail), false);
            if (!timeToken.has_value())
            {
                return std::nullopt;
            }

            const std::optional<CueIndex> index = ParseIndexValue(numberToken->value, timeToken->value);
            if (!index.has_value())
            {
                return std::nullopt;
            }
            if (currentTrack->indexes.size() >= kMaxCueIndexesPerTrack)
            {
                return std::nullopt;
            }

            currentTrack->indexes.push_back(*index);
            continue;
        }

        if (command == "TITLE" || command == "PERFORMER" || command == "SONGWRITER")
        {
            const std::optional<ParsedToken> valueToken = ParseToken(rest, true);
            if (!valueToken.has_value() || !FitsField(valueToken->value))
            {
                return std::nullopt;
            }

            if (currentTrack != nullptr)
            {
                std::string *field = nullptr;
                if (command == "TITLE")
                {
                    field = &currentTrack->title;
                }
                else if (command == "PERFORMER")
                {
                    field = &currentTrack->performer;
                }
                else
                {
                    field = &currentTrack->songwriter;
                }

                if (!StoreValue(*field, valueToken->value))
                {
                    return std::nullopt;
                }
            }
            else if (currentFile != nullptr)
            {
                std::string *field = nullptr;
                if (command == "TITLE")
                {
                    field = &currentFile->title;
                }
                else if (command == "PERFORMER")
                {
                    field = &currentFile->performer;
                }
                else
                {
                    field = &currentFile->songwriter;
                }

                if (!StoreValue(*field, valueToken->value))
                {
                    return std::nullopt;
                }
            }
            else
            {
                std::string *field = nullptr;
                if (command == "TITLE")
                {
                    field = &CurrentGlobal(sheet)->title;
                }
                else if (command == "PERFORMER")
                {
                    field = &CurrentGlobal(sheet)->performer;
                }
                else
                {
                    field = &CurrentGlobal(sheet)->songwriter;
                }

                if (!StoreValue(*field, valueToken->value))
                {
                    return std::nullopt;
                }
            }
            continue;
        }

        if (command == "REM")
        {
            const std::optional<ParsedToken> keyToken = ParseToken(rest, false);
            if (!keyToken.has_value())
            {
                return std::nullopt;
            }

            const std::string key = UpperAscii(keyToken->value);
            const std::optional<ParsedToken> valueToken = ParseToken(TrimLeft(keyToken->tail), true);
            if (!valueToken.has_value())
            {
                return std::nullopt;
            }

            if (key == "GENRE")
            {
                if (!StoreValue(CurrentGlobal(sheet)->genre, valueToken->value))
                {
                    return std::nullopt;
                }
            }
            else if (key == "DATE")
            {
                if (!StoreValue(CurrentGlobal(sheet)->date, valueToken->value))
                {
                    return std::nullopt;
                }
            }
            else if (key == "YEAR")
            {
                if (!StoreValue(CurrentGlobal(sheet)->year, valueToken->value))
                {
                    return std::nullopt;
                }
            }
            else if (key == "DISCNUMBER")
            {
                if (!StoreValue(CurrentGlobal(sheet)->discNumber, valueToken->value))
                {
                    return std::nullopt;
                }
            }

            continue;
        }

        continue;
    }

    return sheet;
}
}
