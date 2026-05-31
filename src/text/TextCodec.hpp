#ifndef TAGREADER_TEXT_TEXTCODEC_HPP
#define TAGREADER_TEXT_TEXTCODEC_HPP

#include "core/RawTagData.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tagreader_text
{
std::string TrimText(std::string value);
bool IsValidUtf8(std::string_view text);
std::string ReadLatin1Text(const uint8_t *data, std::size_t size);
std::string ReadUtf8Text(const uint8_t *data, std::size_t size);
std::string ReadUtf16Text(const uint8_t *data, std::size_t size, bool bigEndian);
std::string ReadUtf16TextWithBom(const uint8_t *data, std::size_t size);
std::string ReadLocaleEncodedText(const uint8_t *data, std::size_t size, std::string_view encoding);
std::string ReadId3ByteString(const uint8_t *data, std::size_t size, uint8_t encoding);
tagreader_core::DecodedField NormalizeText(std::string_view value);
std::string DetectTextEncoding(std::string_view raw);
tagreader_core::DecodedField DecodeTextToUtf8(std::string_view raw, std::string_view encoding);
tagreader_core::DecodedField DecodeRawText(std::string_view raw);
}

#endif
