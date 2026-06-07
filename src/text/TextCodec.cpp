#include "text/TextCodec.hpp"

#include "io/ByteReader.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <limits>

#if defined(TAGREADER_HAS_ICONV)
#include <iconv.h>
#endif

namespace tagreader_text
{
using tagreader_core::DecodedField;
using tagreader_io::ReadBE16;

namespace
{
constexpr std::size_t kMaxTextFieldBytes = 1z * 1024 * 1024;
constexpr std::size_t kMaxDecodedTextBytes = 2z * 1024 * 1024;

bool IsMostlyPrintableText(std::string_view text)
{
    if (text.empty())
    {
        return false;
    }

    std::size_t printable = 0;
    std::size_t suspicious = 0;
    for (unsigned char ch : text)
    {
        if (ch == 0)
        {
            ++suspicious;
            continue;
        }

        if (ch >= 0x20 || ch == '\t' || ch == '\r' || ch == '\n')
        {
            ++printable;
            continue;
        }

        ++suspicious;
    }

    return printable > 0 && printable * 4 >= text.size() * 3 && suspicious * 5 <= text.size();
}

void RemoveUtf8Bom(std::string &value);

bool LooksLikeUtf16WithoutBom(std::string_view raw, bool bigEndian)
{
    // Heuristic: data is likely UTF-16 without BOM when the expected NUL-byte
    // ratio is high. The current threshold (4:3 ≈ 75%) is tighter than the
    // original 3:2 (≈67%) to reduce false positives on ASCII-heavy data that
    // coincidentally exhibits paired low/high byte patterns.
    if (raw.size() < 6 || (raw.size() % 2) != 0)
    {
        return false;
    }

    std::size_t nulOnHighByte = 0;
    std::size_t nulOnLowByte = 0;
    std::size_t asciiLikeUnits = 0;
    std::size_t suspiciousControls = 0;
    std::size_t units = 0;

    for (std::size_t i = 0; i + 1 < raw.size(); i += 2)
    {
        const unsigned char first = static_cast<unsigned char>(raw[i]);
        const unsigned char second = static_cast<unsigned char>(raw[i + 1]);
        const unsigned char high = bigEndian ? first : second;
        const unsigned char low = bigEndian ? second : first;
        const uint16_t codeUnit = bigEndian ? ReadBE16(reinterpret_cast<const uint8_t *>(raw.data() + i)) : static_cast<uint16_t>(first | (static_cast<uint16_t>(second) << 8));
        ++units;

        if (high == 0)
        {
            ++nulOnHighByte;
        }
        if (low == 0)
        {
            ++nulOnLowByte;
        }

        if (codeUnit == 0)
        {
            break;
        }

        if (codeUnit >= 0x20 && codeUnit <= 0x7E)
        {
            ++asciiLikeUnits;
        }
        else if (codeUnit < 0x20 && codeUnit != '\t' && codeUnit != '\r' && codeUnit != '\n')
        {
            ++suspiciousControls;
        }
    }

    if (units < 3)
    {
        return false;
    }

    const std::size_t expectedNuls = bigEndian ? nulOnHighByte : nulOnLowByte;
    const std::size_t unexpectedNuls = bigEndian ? nulOnLowByte : nulOnHighByte;
    // Threshold tightened from 3:2 to 4:3 to reduce false positives on ASCII-heavy data
    if (expectedNuls * 4 < units * 3)
    {
        return false;
    }
    if (unexpectedNuls * 4 > units)
    {
        return false;
    }
    if (asciiLikeUnits == 0)
    {
        return false;
    }

    return suspiciousControls * 4 <= units;
}

std::string DetectLegacyLocalEncoding(std::string_view raw)
{
#if defined(TAGREADER_HAS_ICONV)
    constexpr std::array<std::string_view, 8> candidates{
        "GB18030",
        "GBK",
        "SHIFT_JIS",
        "CP932",
        "BIG5",
        "WINDOWS-1252",
        "WINDOWS-1251",
        "WINDOWS-1250",
    };

    for (std::string_view candidate : candidates)
    {
        const std::string decoded = ReadLocaleEncodedText(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), candidate);
        if (!decoded.empty() && IsMostlyPrintableText(decoded))
        {
            return std::string(candidate);
        }
    }
#else
    (void)raw;
#warning "CRITICAL LIMITATION: Building without iconv — encoding detection disabled. "
         "Affected encodings: GB18030, GBK, SHIFT_JIS, BIG5, CP932, WINDOWS-1252, "
         "WINDOWS-1251, WINDOWS-1250. All non-BOM, non-UTF-8, non-obvious-UTF-16 "
         "text falls back to Latin-1. CJK text will produce mojibake. "
         "Enable iconv for production use."
#endif

    // CRITICAL LIMITATION: Without iconv, GB18030/GBK/SHIFT_JIS/BIG5/CP932/WINDOWS-1252
    // encoding detection is skipped entirely.
    // Impact: All non-BOM, non-UTF-8, non-obvious-UTF-16 text falls back to Latin-1.
    // CJK (Chinese/Japanese/Korean) and Cyrillic/Central European text may produce
    // mojibake in this build configuration. Enable iconv for production use.
    return "latin-1";
}

#if defined(TAGREADER_HAS_ICONV)
class IconvHandle
{
public:
    explicit IconvHandle(iconv_t cd) noexcept : cd_(cd)
    {
    }

    ~IconvHandle()
    {
        if (cd_ != reinterpret_cast<iconv_t>(-1))
        {
            iconv_close(cd_);
        }
    }

    IconvHandle(const IconvHandle &) = delete;
    IconvHandle &operator=(const IconvHandle &) = delete;

    iconv_t get() const noexcept
    {
        return cd_;
    }

private:
    iconv_t cd_;
};

std::string ConvertTextWithIconv(const uint8_t *data, std::size_t size, const char *encoding)
{
    if (data == nullptr || size == 0 || encoding == nullptr || *encoding == '\0')
    {
        return {};
    }

    if (size > kMaxDecodedTextBytes / 4)
    {
        return {};
    }

    IconvHandle cd(iconv_open("UTF-8", encoding));
    if (cd.get() == reinterpret_cast<iconv_t>(-1))
    {
        return {};
    }

    std::string output(std::min(kMaxDecodedTextBytes, std::max<std::size_t>(size * 4, 64)), '\0');
    const char *inputData = reinterpret_cast<const char *>(data);
    std::size_t inputLeft = size;

    char *outputData = output.data();
    std::size_t outputLeft = output.size();

    while (inputLeft > 0)
    {
        const std::size_t result = iconv(cd.get(), const_cast<char **>(&inputData), &inputLeft, &outputData, &outputLeft);
        if (result != static_cast<std::size_t>(-1))
        {
            continue;
        }

        if (errno == E2BIG)
        {
            const std::size_t used = output.size() - outputLeft;
            if (output.size() > kMaxDecodedTextBytes / 2)
            {
                return {};
            }
            const std::size_t nextSize = output.size() * 2;
            if (nextSize > kMaxDecodedTextBytes)
            {
                return {};
            }
            output.resize(nextSize, '\0');
            outputData = output.data() + used;
            outputLeft = output.size() - used;
            continue;
        }

        return {};
    }

    output.resize(output.size() - outputLeft);
    RemoveUtf8Bom(output);
    output = TrimText(std::move(output));
    return IsValidUtf8(output) ? output : std::string{};
}
#endif

bool TryReadUtf16Text(const uint8_t *data, std::size_t size, bool defaultBigEndian, std::string &value)
{
    value.clear();
    if (data == nullptr)
    {
        return false;
    }

    value.reserve(size);
    std::size_t start = 0;
    bool bigEndian = defaultBigEndian;
    if (size >= 2)
    {
        if (data[0] == 0xFE && data[1] == 0xFF)
        {
            start = 2;
            bigEndian = true;
        }
        else if (data[0] == 0xFF && data[1] == 0xFE)
        {
            start = 2;
            bigEndian = false;
        }
    }

    if ((size - start) % 2 != 0)
    {
        return false;
    }

    const auto appendChecked = [&value](std::string_view chunk)
    {
        if (value.size() > kMaxDecodedTextBytes || chunk.size() > kMaxDecodedTextBytes - value.size())
        {
            value.clear();
            return false;
        }
        value.append(chunk);
        return true;
    };

    for (std::size_t i = start; i + 1 < size; i += 2)
    {
        const uint16_t ch = bigEndian ? ReadBE16(data + i) : static_cast<uint16_t>(data[i] | (static_cast<uint16_t>(data[i + 1]) << 8));
        if (ch == 0)
        {
            break;
        }

        if (ch >= 0xD800 && ch <= 0xDBFF)
        {
            if (i + 3 >= size)
            {
                value.clear();
                return false;
            }

            const uint16_t low = bigEndian ? ReadBE16(data + i + 2) : static_cast<uint16_t>(data[i + 2] | (static_cast<uint16_t>(data[i + 3]) << 8));
            if (low < 0xDC00 || low > 0xDFFF)
            {
                value.clear();
                return false;
            }

            const uint32_t codePoint = 0x10000 + (((static_cast<uint32_t>(ch) - 0xD800) << 10) | (static_cast<uint32_t>(low) - 0xDC00));
            const std::array<char, 4> chunk{
                static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)),
                static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)),
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)),
                static_cast<char>(0x80 | (codePoint & 0x3F)),
            };
            if (!appendChecked(std::string_view(chunk.data(), chunk.size())))
            {
                return false;
            }
            i += 2;
            continue;
        }
        if (ch >= 0xDC00 && ch <= 0xDFFF)
        {
            value.clear();
            return false;
        }

        if (ch < 0x80)
        {
            const char chunk = static_cast<char>(ch);
            if (!appendChecked(std::string_view(&chunk, 1)))
            {
                return false;
            }
        }
        else if (ch < 0x800)
        {
            const std::array<char, 2> chunk{
                static_cast<char>(0xC0 | (ch >> 6)),
                static_cast<char>(0x80 | (ch & 0x3F)),
            };
            if (!appendChecked(std::string_view(chunk.data(), chunk.size())))
            {
                return false;
            }
        }
        else
        {
            const std::array<char, 3> chunk{
                static_cast<char>(0xE0 | (ch >> 12)),
                static_cast<char>(0x80 | ((ch >> 6) & 0x3F)),
                static_cast<char>(0x80 | (ch & 0x3F)),
            };
            if (!appendChecked(std::string_view(chunk.data(), chunk.size())))
            {
                return false;
            }
        }
    }

    value = TrimText(std::move(value));
    return true;
}

void RemoveUtf8Bom(std::string &value)
{
    if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF && static_cast<unsigned char>(value[1]) == 0xBB && static_cast<unsigned char>(value[2]) == 0xBF)
    {
        value.erase(0, 3);
    }
}
}

std::string TrimText(std::string value)
{
    const auto isTrimChar = [](unsigned char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0';
    };

    const auto first = std::find_if_not(value.begin(), value.end(), isTrimChar);
    if (first == value.end())
    {
        return {};
    }

    const auto last = std::find_if_not(value.rbegin(), value.rend(), isTrimChar).base();
    return std::string(first, last);
}

bool IsValidUtf8(std::string_view text)
{
    const auto *ptr = reinterpret_cast<const unsigned char *>(text.data());
    std::size_t i = 0;
    while (i < text.size())
    {
        const unsigned char c = ptr[i];
        if (c <= 0x7F)
        {
            ++i;
            continue;
        }

        uint32_t codePoint = 0;
        std::size_t need = 0;
        if ((c & 0xE0) == 0xC0)
        {
            codePoint = c & 0x1F;
            need = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            codePoint = c & 0x0F;
            need = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            codePoint = c & 0x07;
            need = 3;
        }
        else
        {
            return false;
        }

        if (i + need >= text.size())
        {
            return false;
        }

        for (std::size_t j = 1; j <= need; ++j)
        {
            const unsigned char tail = ptr[i + j];
            if ((tail & 0xC0) != 0x80)
            {
                return false;
            }
            codePoint = (codePoint << 6) | (tail & 0x3F);
        }

        if ((need == 1 && codePoint < 0x80) || (need == 2 && codePoint < 0x800) || (need == 3 && codePoint < 0x10000) || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        {
            return false;
        }

        i += need + 1;
    }

    return true;
}

std::string ReadLatin1Text(const uint8_t *data, std::size_t size)
{
    if (data == nullptr)
    {
        return {};
    }

    std::string value;
    value.reserve(std::min(kMaxDecodedTextBytes, size));
    for (std::size_t i = 0; i < size; ++i)
    {
        const unsigned char ch = data[i];
        // Latin-1 0x00 bytes are replaced with spaces to preserve
        // surrounding text. Structural delimiters are handled by
        // FindEncodedTerminator() before reaching this function.
        if (ch == 0)
        {
            value.push_back(' ');
            continue;
        }

        if (ch < 0x80)
        {
            if (value.size() >= kMaxDecodedTextBytes)
            {
                return {};
            }
            value.push_back(static_cast<char>(ch));
        }
        else
        {
            if (value.size() > kMaxDecodedTextBytes - 2)
            {
                return {};
            }
            value.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            value.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return TrimText(std::move(value));
}

std::string ReadUtf8Text(const uint8_t *data, std::size_t size)
{
    if (data == nullptr)
    {
        return {};
    }

    std::string value;
    value.reserve(std::min(kMaxDecodedTextBytes, size));
    for (std::size_t i = 0; i < size; ++i)
    {
        const unsigned char ch = data[i];
        if (ch == 0)
        {
            value.push_back(' ');
            continue;
        }
        if (value.size() >= kMaxDecodedTextBytes)
        {
            return {};
        }
        value.push_back(static_cast<char>(ch));
    }
    return TrimText(std::move(value));
}

std::string ReadUtf16Text(const uint8_t *data, std::size_t size, bool bigEndian)
{
    std::string value;
    if (!TryReadUtf16Text(data, size, bigEndian, value))
    {
        return {};
    }
    return value;
}

std::string ReadUtf16TextWithBom(const uint8_t *data, std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        return {};
    }

    if (size >= 2)
    {
        if (data[0] == 0xFE && data[1] == 0xFF)
        {
            return ReadUtf16Text(data, size, true);
        }
        if (data[0] == 0xFF && data[1] == 0xFE)
        {
            return ReadUtf16Text(data, size, false);
        }
    }

    return {};
}

std::string ReadLocaleEncodedText(const uint8_t *data, std::size_t size, std::string_view encoding)
{
    if (data == nullptr || size == 0)
    {
        return {};
    }

#if defined(TAGREADER_HAS_ICONV)
    std::string encodingName(encoding);
    std::string decoded = ConvertTextWithIconv(data, size, encodingName.c_str());
    if (!decoded.empty())
    {
        return decoded;
    }
#else
    (void)encoding;
#endif

    return {};
}

std::string ReadId3ByteString(const uint8_t *data, std::size_t size, uint8_t encoding)
{
    switch (encoding)
    {
    case 0:
        return ReadLatin1Text(data, size);
    case 1:
        return ReadUtf16TextWithBom(data, size);
    case 2:
        return ReadUtf16Text(data, size, true);
    case 3:
    {
        const std::string utf8 = ReadUtf8Text(data, size);
        return IsValidUtf8(utf8) ? utf8 : std::string{};
    }
    default:
        return {};
    }
}

DecodedField NormalizeText(std::string_view value)
{
    return DecodeRawText(value);
}

std::string DetectTextEncoding(std::string_view raw)
{
    if (raw.empty())
    {
        return "utf-8";
    }

    const auto byteAt = [&](std::size_t index)
    {
        return static_cast<unsigned char>(raw[index]);
    };

    if (raw.size() >= 3 && byteAt(0) == 0xEF && byteAt(1) == 0xBB && byteAt(2) == 0xBF)
    {
        return "utf-8";
    }
    if (raw.size() >= 2 && byteAt(0) == 0xFF && byteAt(1) == 0xFE)
    {
        return "utf-16le";
    }
    if (raw.size() >= 2 && byteAt(0) == 0xFE && byteAt(1) == 0xFF)
    {
        return "utf-16be";
    }

    if (IsValidUtf8(raw))
    {
        return "utf-8";
    }

    if (LooksLikeUtf16WithoutBom(raw, false))
    {
        return "utf-16le";
    }
    if (LooksLikeUtf16WithoutBom(raw, true))
    {
        return "utf-16be";
    }

    return DetectLegacyLocalEncoding(raw);
}

DecodedField DecodeTextToUtf8(std::string_view raw, std::string_view encoding)
{
    DecodedField field{};
    field.encoding.assign(encoding.begin(), encoding.end());

    const auto fail = [&field]()
    {
        field.value.clear();
        field.success = false;
        return field;
    };

    if (raw.size() > kMaxTextFieldBytes)
    {
        return fail();
    }

    if (encoding == "utf-8")
    {
        field.value.assign(raw.begin(), raw.end());
        RemoveUtf8Bom(field.value);
        field.value = TrimText(std::move(field.value));
        field.success = IsValidUtf8(field.value);
        if (!field.success)
        {
            return fail();
        }
        return field;
    }

    if (encoding == "utf-16le" || encoding == "utf-16be")
    {
        field.value = ReadUtf16Text(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), encoding == "utf-16be");
        field.success = !field.value.empty() || raw.empty();
        if (field.success)
        {
            field.success = IsValidUtf8(field.value);
        }
        if (!field.success)
        {
            return fail();
        }
        return field;
    }

    if (encoding == "latin-1")
    {
        field.value = ReadLatin1Text(reinterpret_cast<const uint8_t *>(raw.data()), raw.size());
        field.success = IsValidUtf8(field.value);
        if (!field.success)
        {
            return fail();
        }
        return field;
    }

    field.value = ReadLocaleEncodedText(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), encoding);
    field.success = IsValidUtf8(field.value) && IsMostlyPrintableText(field.value);
    if (!field.success)
    {
        return fail();
    }
    return field;
}

DecodedField DecodeRawText(std::string_view raw)
{
    const std::string encoding = DetectTextEncoding(raw);
    return DecodeTextToUtf8(raw, encoding);
}
}
