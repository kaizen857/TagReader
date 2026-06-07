#pragma once

#include "text/TextCodec.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace tagreader_common {

using tagreader_text::TrimText;

// Parse uint16 from string. Strict mode: entire string (after trim) must be consumed.
// Returns 0 on failure. Max value 65535.
inline std::uint16_t ParseUInt16(const std::string &value)
{
    const std::string trimmed = TrimText(value);
    if (trimmed.empty())
    {
        return 0;
    }
    try
    {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(trimmed, &consumed, 10);
        if (consumed != trimmed.size())
        {
            return 0;
        }
        if (parsed > 65535)
        {
            return 0;
        }
        return static_cast<std::uint16_t>(parsed);
    }
    catch (...)
    {
        return 0;
    }
}

// Parse "N/M" or plain "N" track/disc number format.
// Returns {current, total} where total = 0 if no slash separator.
// Both sides are trimmed and validated via ParseUInt16.
inline std::pair<std::uint16_t, std::uint16_t> ParseSlashNumber(const std::string &value)
{
    const auto slash = value.find('/');
    if (slash == std::string::npos)
    {
        return {ParseUInt16(value), 0};
    }

    const std::string left = TrimText(value.substr(0, slash));
    const std::string right = TrimText(value.substr(slash + 1));
    if (left.empty() || right.empty())
    {
        return {0, 0};
    }

    const std::uint16_t current = ParseUInt16(left);
    const std::uint16_t total = ParseUInt16(right);
    if (current == 0 || total == 0)
    {
        return {0, 0};
    }

    return {current, total};
}

// Extract first 4-digit year from a date string. Returns 0 if not found.
inline std::uint16_t ParseYearOnly(std::string_view text)
{
    while (!text.empty())
    {
        const unsigned char ch = static_cast<unsigned char>(text.front());
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0')
        {
            text.remove_prefix(1);
            continue;
        }
        break;
    }

    if (text.size() < 4)
    {
        return 0;
    }

    if (!std::isdigit(static_cast<unsigned char>(text[0])) ||
        !std::isdigit(static_cast<unsigned char>(text[1])) ||
        !std::isdigit(static_cast<unsigned char>(text[2])) ||
        !std::isdigit(static_cast<unsigned char>(text[3])))
    {
        return 0;
    }

    if (text.size() > 4)
    {
        const unsigned char next = static_cast<unsigned char>(text[4]);
        if (std::isdigit(next))
        {
            return 0;
        }

        const bool allowedSeparator = next == '-' || next == '/' || next == '.' ||
                                      next == ' ' || next == 'T' || next == '\0';
        if (!allowedSeparator)
        {
            return 0;
        }
    }

    const std::uint16_t year = static_cast<std::uint16_t>(
        (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
        (text[2] - '0') * 10 + (text[3] - '0'));
    return (year >= 1000 && year <= 9999) ? year : 0;
}

// Convert string to lowercase (returns new string).
inline std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// Case-insensitive string equality.
inline bool IEquals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char ca, unsigned char cb) { return std::tolower(ca) == std::tolower(cb); });
}

} // namespace tagreader_common
