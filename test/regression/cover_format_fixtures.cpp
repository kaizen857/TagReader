#include "cover_format_fixtures.hpp"

#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tagreader_test_support
{
namespace
{
constexpr std::array<std::uint8_t, 16> kAsfHeaderGuid{0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                                       0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr std::array<std::uint8_t, 16> kAsfExtendedContentDescriptionGuid{0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11,
                                                                           0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50};
constexpr std::uint64_t kSegmentId = 0x18538067;
constexpr std::uint64_t kAttachmentsId = 0x1941A469;
constexpr std::uint64_t kAttachedFileId = 0x61A7;

void AppendU16LE(std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void AppendU32LE(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void AppendU64LE(std::vector<std::uint8_t> &bytes, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
    {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::uint32_t ReadU32BE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint32_t ReadU32LE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadU64LE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        value = (value << 8) | bytes[offset + static_cast<std::size_t>(shift / 8)];
    }
    return value;
}

std::vector<std::uint8_t> Utf16LeBytes(std::string_view text)
{
    std::vector<std::uint8_t> bytes;
    for (unsigned char ch : text)
    {
        bytes.push_back(ch);
        bytes.push_back(0);
    }
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

bool RunFfmpeg(const std::string &command)
{
    return CommandSucceeds(command);
}

std::vector<std::uint8_t> FlacPicturePayload(const std::vector<std::uint8_t> &imageBytes)
{
    std::vector<std::uint8_t> payload;
    AppendU32BE(payload, 3);
    AppendU32BE(payload, 9);
    AppendBytes(payload, "image/png");
    AppendU32BE(payload, 0);
    AppendU32BE(payload, 1);
    AppendU32BE(payload, 1);
    AppendU32BE(payload, 32);
    AppendU32BE(payload, 0);
    AppendU32BE(payload, static_cast<std::uint32_t>(imageBytes.size()));
    payload.insert(payload.end(), imageBytes.begin(), imageBytes.end());
    return payload;
}

void AppendFlacMetadataBlock(std::vector<std::uint8_t> &output, std::uint8_t blockType, bool lastBlock, const std::vector<std::uint8_t> &payload)
{
    output.push_back(static_cast<std::uint8_t>((lastBlock ? 0x80 : 0x00) | (blockType & 0x7F)));
    AppendU24BE(output, static_cast<std::uint32_t>(payload.size()));
    output.insert(output.end(), payload.begin(), payload.end());
}

bool InjectFlacPictureBlock(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &picture)
{
    const std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.size() < 42 || std::string_view(reinterpret_cast<const char *>(data.data()), 4) != "fLaC")
    {
        return false;
    }
    if (picture.size() > 0xFFFFFFU)
    {
        return false;
    }

    std::size_t cursor = 4;
    std::size_t audioStart = data.size();
    bool foundStreamInfo = false;
    std::vector<std::uint8_t> output{'f', 'L', 'a', 'C'};

    while (cursor + 4 <= data.size())
    {
        const bool lastBlock = (data[cursor] & 0x80) != 0;
        const std::uint8_t blockType = data[cursor] & 0x7F;
        const std::uint32_t blockSize = ReadU24BE(data, cursor + 1);
        const std::size_t blockPayload = cursor + 4;
        const std::size_t blockEnd = blockPayload + blockSize;
        if (blockEnd > data.size())
        {
            break;
        }

        foundStreamInfo = foundStreamInfo || blockType == 0;
        output.push_back(blockType);
        AppendU24BE(output, blockSize);
        output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(blockPayload), data.begin() + static_cast<std::ptrdiff_t>(blockEnd));

        cursor = blockEnd;
        if (lastBlock)
        {
            audioStart = cursor;
            break;
        }
    }

    if (!foundStreamInfo || audioStart > data.size())
    {
        return false;
    }

    AppendFlacMetadataBlock(output, 6, true, picture);
    output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(audioStart), data.end());
    return WriteBinaryFile(outputPath, output);
}

std::string Base64Encode(const std::vector<std::uint8_t> &bytes)
{
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                     (i + 1 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0) |
                                     (i + 2 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 2]) : 0);
        output.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        output.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        output.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        output.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 0x3F] : '=');
    }
    return output;
}

std::vector<std::uint8_t> VorbisCommentPayload(const std::vector<std::string> &comments)
{
    std::vector<std::uint8_t> payload;
    constexpr std::string_view kVendor = "tagreader-format-matrix";
    AppendU32LE(payload, static_cast<std::uint32_t>(kVendor.size()));
    AppendBytes(payload, kVendor);
    AppendU32LE(payload, static_cast<std::uint32_t>(comments.size()));
    for (const std::string &comment : comments)
    {
        AppendU32LE(payload, static_cast<std::uint32_t>(comment.size()));
        AppendBytes(payload, comment);
    }
    return payload;
}

std::vector<std::uint8_t> VorbisCommentPacket(const std::vector<std::string> &comments)
{
    std::vector<std::uint8_t> packet{0x03, 'v', 'o', 'r', 'b', 'i', 's'};
    const std::vector<std::uint8_t> payload = VorbisCommentPayload(comments);
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(1);
    return packet;
}

std::vector<std::uint8_t> OpusTagsPacket(const std::vector<std::string> &comments)
{
    std::vector<std::uint8_t> packet{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    const std::vector<std::uint8_t> payload = VorbisCommentPayload(comments);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::uint32_t OggCrc(const std::vector<std::uint8_t> &bytes)
{
    std::uint32_t crc = 0;
    for (std::uint8_t byte : bytes)
    {
        crc ^= static_cast<std::uint32_t>(byte) << 24;
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x80000000U) != 0 ? (crc << 1) ^ 0x04C11DB7U : crc << 1;
        }
    }
    return crc;
}

bool RebuildOggCommentPage(const std::filesystem::path &basePath, const std::filesystem::path &outputPath,
                           std::string_view packetMagic, const std::vector<std::uint8_t> &replacementPacket)
{
    const std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.empty())
    {
        return false;
    }

    std::size_t cursor = 0;
    while (cursor + 27 <= data.size())
    {
        if (std::string_view(reinterpret_cast<const char *>(data.data() + cursor), 4) != "OggS")
        {
            break;
        }
        const std::uint8_t segmentCount = data[cursor + 26];
        if (cursor + 27 + segmentCount > data.size())
        {
            break;
        }

        std::size_t payloadSize = 0;
        for (std::size_t i = 0; i < segmentCount; ++i)
        {
            payloadSize += data[cursor + 27 + i];
        }
        const std::size_t payloadOffset = cursor + 27 + segmentCount;
        const std::size_t pageEnd = payloadOffset + payloadSize;
        if (pageEnd > data.size())
        {
            break;
        }

        std::size_t consumed = 0;
        std::size_t packetSegmentCount = 0;
        for (; packetSegmentCount < segmentCount; ++packetSegmentCount)
        {
            const std::uint8_t segmentLength = data[cursor + 27 + packetSegmentCount];
            consumed += segmentLength;
            if (segmentLength < 255)
            {
                ++packetSegmentCount;
                break;
            }
        }
        if (consumed > payloadSize)
        {
            cursor = pageEnd;
            continue;
        }

        const bool matchesMagic = consumed >= packetMagic.size() &&
                                  std::string_view(reinterpret_cast<const char *>(data.data() + payloadOffset), packetMagic.size()) == packetMagic;
        if (matchesMagic)
        {
            std::vector<std::uint8_t> newLacing;
            std::size_t remaining = replacementPacket.size();
            while (remaining >= 255)
            {
                newLacing.push_back(255);
                remaining -= 255;
            }
            if (replacementPacket.empty() || remaining > 0)
            {
                newLacing.push_back(static_cast<std::uint8_t>(remaining));
            }
            for (std::size_t i = packetSegmentCount; i < segmentCount; ++i)
            {
                newLacing.push_back(data[cursor + 27 + i]);
            }

            std::vector<std::uint8_t> newPayload = replacementPacket;
            newPayload.insert(newPayload.end(), data.begin() + static_cast<std::ptrdiff_t>(payloadOffset + consumed),
                              data.begin() + static_cast<std::ptrdiff_t>(pageEnd));

            std::vector<std::uint8_t> newPage(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                                              data.begin() + static_cast<std::ptrdiff_t>(cursor + 27));
            newPage.insert(newPage.end(), newLacing.begin(), newLacing.end());
            newPage.insert(newPage.end(), newPayload.begin(), newPayload.end());
            newPage[22] = 0;
            newPage[23] = 0;
            newPage[24] = 0;
            newPage[25] = 0;
            const std::uint32_t crc = OggCrc(newPage);
            newPage[22] = static_cast<std::uint8_t>(crc & 0xFF);
            newPage[23] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
            newPage[24] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
            newPage[25] = static_cast<std::uint8_t>((crc >> 24) & 0xFF);

            std::vector<std::uint8_t> output(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(cursor));
            output.insert(output.end(), newPage.begin(), newPage.end());
            output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(pageEnd), data.end());
            return WriteBinaryFile(outputPath, output);
        }

        cursor = pageEnd;
    }
    return false;
}

std::string PictureCommentEntry(const std::vector<std::uint8_t> &imageBytes)
{
    return "METADATA_BLOCK_PICTURE=" + Base64Encode(FlacPicturePayload(imageBytes));
}

std::vector<std::uint8_t> Atom(std::array<std::uint8_t, 4> type, const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes;
    AppendU32BE(bytes, static_cast<std::uint32_t>(payload.size() + 8));
    bytes.insert(bytes.end(), type.begin(), type.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> Mp4CoverItem(const std::vector<std::uint8_t> &coverBytes)
{
    std::vector<std::uint8_t> dataAtomPayload;
    AppendU32BE(dataAtomPayload, 13);
    AppendU32BE(dataAtomPayload, 0);
    dataAtomPayload.insert(dataAtomPayload.end(), coverBytes.begin(), coverBytes.end());
    return Atom({'c', 'o', 'v', 'r'}, Atom({'d', 'a', 't', 'a'}, dataAtomPayload));
}

std::vector<std::uint8_t> Mp4UdtaWithIlst(const std::vector<std::uint8_t> &ilstPayload)
{
    std::vector<std::uint8_t> metaPayload{0, 0, 0, 0};
    const std::vector<std::uint8_t> ilst = Atom({'i', 'l', 's', 't'}, ilstPayload);
    metaPayload.insert(metaPayload.end(), ilst.begin(), ilst.end());
    return Atom({'u', 'd', 't', 'a'}, Atom({'m', 'e', 't', 'a'}, metaPayload));
}

bool InjectMp4Ilst(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &ilstPayload)
{
    const std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.empty())
    {
        return false;
    }

    const std::vector<std::uint8_t> udta = Mp4UdtaWithIlst(ilstPayload);
    std::vector<std::uint8_t> output;
    std::size_t cursor = 0;
    bool injected = false;

    while (cursor + 8 <= data.size())
    {
        const std::uint32_t atomSize = ReadU32BE(data, cursor);
        if (atomSize < 8 || cursor + atomSize > data.size())
        {
            break;
        }
        const std::array<std::uint8_t, 4> atomType{data[cursor + 4], data[cursor + 5], data[cursor + 6], data[cursor + 7]};
        if (atomType == std::array<std::uint8_t, 4>{'m', 'o', 'o', 'v'} && !injected)
        {
            std::vector<std::uint8_t> moovPayload(data.begin() + static_cast<std::ptrdiff_t>(cursor + 8),
                                                  data.begin() + static_cast<std::ptrdiff_t>(cursor + atomSize));
            moovPayload.insert(moovPayload.end(), udta.begin(), udta.end());
            const std::vector<std::uint8_t> moov = Atom({'m', 'o', 'o', 'v'}, moovPayload);
            output.insert(output.end(), moov.begin(), moov.end());
            injected = true;
        }
        else
        {
            output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor),
                          data.begin() + static_cast<std::ptrdiff_t>(cursor + atomSize));
        }
        cursor += atomSize;
    }

    output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor), data.end());
    if (!injected)
    {
        const std::vector<std::uint8_t> moov = Atom({'m', 'o', 'o', 'v'}, udta);
        output.insert(output.end(), moov.begin(), moov.end());
    }
    return WriteBinaryFile(outputPath, output);
}

struct ApeItem
{
    std::string key;
    std::vector<std::uint8_t> value;
    bool isBinary = false;
};

std::vector<std::uint8_t> ApeTag(const std::vector<ApeItem> &items)
{
    std::vector<std::uint8_t> itemBytes;
    for (const ApeItem &item : items)
    {
        AppendU32LE(itemBytes, static_cast<std::uint32_t>(item.value.size()));
        AppendU32LE(itemBytes, item.isBinary ? (1U << 1U) : 0U);
        itemBytes.insert(itemBytes.end(), item.key.begin(), item.key.end());
        itemBytes.push_back(0);
        itemBytes.insert(itemBytes.end(), item.value.begin(), item.value.end());
    }
    const std::uint32_t tagSize = 32 + static_cast<std::uint32_t>(itemBytes.size());

    std::vector<std::uint8_t> tag;
    tag.insert(tag.end(), {'A', 'P', 'E', 'T', 'A', 'G', 'E', 'X'});
    AppendU32LE(tag, 2000);
    AppendU32LE(tag, tagSize);
    AppendU32LE(tag, static_cast<std::uint32_t>(items.size()));
    AppendU32LE(tag, 0x80000000U);
    AppendU32LE(tag, 0);
    AppendU32LE(tag, 0);
    tag.insert(tag.end(), itemBytes.begin(), itemBytes.end());
    tag.insert(tag.end(), {'A', 'P', 'E', 'T', 'A', 'G', 'E', 'X'});
    AppendU32LE(tag, 2000);
    AppendU32LE(tag, tagSize);
    AppendU32LE(tag, static_cast<std::uint32_t>(items.size()));
    AppendU32LE(tag, 0x80000000U);
    AppendU32LE(tag, 0);
    AppendU32LE(tag, 0);
    return tag;
}

std::vector<std::uint8_t> Id3v23Frame(std::string_view frameId, const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes;
    AppendBytes(bytes, frameId);
    AppendU32BE(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> Id3v23Tag(const std::vector<std::uint8_t> &frames)
{
    std::vector<std::uint8_t> bytes{'I', 'D', '3', 3, 0, 0};
    const std::uint32_t size = static_cast<std::uint32_t>(frames.size());
    bytes.push_back(static_cast<std::uint8_t>((size >> 21) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((size >> 14) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((size >> 7) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>(size & 0x7F));
    bytes.insert(bytes.end(), frames.begin(), frames.end());
    return bytes;
}

std::vector<std::uint8_t> ApicPayload(const std::vector<std::uint8_t> &imageBytes)
{
    std::vector<std::uint8_t> payload{0};
    AppendBytes(payload, "image/png");
    payload.insert(payload.end(), {0, 3, 0});
    payload.insert(payload.end(), imageBytes.begin(), imageBytes.end());
    return payload;
}

bool AppendRiffId3Chunk(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &id3Tag)
{
    std::vector<std::uint8_t> bytes = ReadBinaryFile(basePath);
    if (bytes.size() < 12 || std::string_view(reinterpret_cast<const char *>(bytes.data()), 4) != "RIFF" ||
        std::string_view(reinterpret_cast<const char *>(bytes.data() + 8), 4) != "WAVE")
    {
        return false;
    }

    std::vector<std::uint8_t> chunk;
    AppendBytes(chunk, "id3 ");
    AppendU32LE(chunk, static_cast<std::uint32_t>(id3Tag.size()));
    chunk.insert(chunk.end(), id3Tag.begin(), id3Tag.end());
    if ((id3Tag.size() % 2) != 0)
    {
        chunk.push_back(0);
    }
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());

    const std::uint32_t riffSize = static_cast<std::uint32_t>(bytes.size() - 8);
    bytes[4] = static_cast<std::uint8_t>(riffSize & 0xFF);
    bytes[5] = static_cast<std::uint8_t>((riffSize >> 8) & 0xFF);
    bytes[6] = static_cast<std::uint8_t>((riffSize >> 16) & 0xFF);
    bytes[7] = static_cast<std::uint8_t>((riffSize >> 24) & 0xFF);
    return WriteBinaryFile(outputPath, bytes);
}

std::vector<std::uint8_t> AsfObject(const std::array<std::uint8_t, 16> &guid, const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes(guid.begin(), guid.end());
    AppendU64LE(bytes, static_cast<std::uint64_t>(payload.size() + 24));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> AsfExtendedContentDescriptionWithPicture(const std::vector<std::uint8_t> &imageBytes)
{
    const std::string name = "WM/Picture";
    const std::vector<std::uint8_t> nameUtf16(name.begin(), name.end());
    std::vector<std::uint8_t> descriptor;
    AppendU16LE(descriptor, static_cast<std::uint16_t>(nameUtf16.size() * 2));
    for (const std::uint8_t byte : nameUtf16)
    {
        descriptor.push_back(byte);
        descriptor.push_back(0);
    }
    AppendU16LE(descriptor, 1);

    std::vector<std::uint8_t> pictureValue;
    pictureValue.push_back(3);
    AppendU32LE(pictureValue, static_cast<std::uint32_t>(imageBytes.size()));
    const std::vector<std::uint8_t> mime = Utf16LeBytes("image/png");
    const std::vector<std::uint8_t> description = Utf16LeBytes("front cover");
    pictureValue.insert(pictureValue.end(), mime.begin(), mime.end());
    pictureValue.insert(pictureValue.end(), description.begin(), description.end());
    pictureValue.insert(pictureValue.end(), imageBytes.begin(), imageBytes.end());
    AppendU16LE(descriptor, static_cast<std::uint16_t>(pictureValue.size()));
    descriptor.insert(descriptor.end(), pictureValue.begin(), pictureValue.end());

    std::vector<std::uint8_t> payload;
    AppendU16LE(payload, 1);
    payload.insert(payload.end(), descriptor.begin(), descriptor.end());
    return AsfObject(kAsfExtendedContentDescriptionGuid, payload);
}

bool InjectAsfPictureDescriptor(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &imageBytes)
{
    std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.size() < 30 ||
        !std::equal(kAsfHeaderGuid.begin(), kAsfHeaderGuid.end(), data.begin()))
    {
        return false;
    }
    const std::uint64_t headerSize = ReadU64LE(data, 16);
    const std::uint32_t childCount = ReadU32LE(data, 24);
    const std::vector<std::uint8_t> xcd = AsfExtendedContentDescriptionWithPicture(imageBytes);

    const std::uint64_t newHeaderSize = headerSize + xcd.size();
    data[16] = static_cast<std::uint8_t>(newHeaderSize & 0xFF);
    data[17] = static_cast<std::uint8_t>((newHeaderSize >> 8) & 0xFF);
    data[18] = static_cast<std::uint8_t>((newHeaderSize >> 16) & 0xFF);
    data[19] = static_cast<std::uint8_t>((newHeaderSize >> 24) & 0xFF);
    data[20] = static_cast<std::uint8_t>((newHeaderSize >> 32) & 0xFF);
    data[21] = static_cast<std::uint8_t>((newHeaderSize >> 40) & 0xFF);
    data[22] = static_cast<std::uint8_t>((newHeaderSize >> 48) & 0xFF);
    data[23] = static_cast<std::uint8_t>((newHeaderSize >> 56) & 0xFF);
    data[24] = static_cast<std::uint8_t>((childCount + 1) & 0xFF);
    data[25] = static_cast<std::uint8_t>(((childCount + 1) >> 8) & 0xFF);
    data[26] = static_cast<std::uint8_t>(((childCount + 1) >> 16) & 0xFF);
    data[27] = static_cast<std::uint8_t>(((childCount + 1) >> 24) & 0xFF);
    data.insert(data.begin() + 30, xcd.begin(), xcd.end());
    return WriteBinaryFile(outputPath, data);
}

std::vector<std::uint8_t> MatroskaId(std::uint64_t id)
{
    std::vector<std::uint8_t> bytes;
    bool started = false;
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        const auto byte = static_cast<std::uint8_t>((id >> shift) & 0xFFU);
        if (byte != 0 || started)
        {
            bytes.push_back(byte);
            started = true;
        }
    }
    if (bytes.empty())
    {
        bytes.push_back(0x80);
    }
    return bytes;
}

void AppendMatroskaSize(std::vector<std::uint8_t> &bytes, std::uint64_t size)
{
    if (size <= 0x7FULL)
    {
        bytes.push_back(static_cast<std::uint8_t>(0x80 | size));
        return;
    }
    if (size <= 0x3FFFULL)
    {
        bytes.push_back(static_cast<std::uint8_t>(0x40 | ((size >> 8) & 0x3F)));
        bytes.push_back(static_cast<std::uint8_t>(size & 0xFF));
        return;
    }
    if (size <= 0x1FFFFFULL)
    {
        bytes.push_back(static_cast<std::uint8_t>(0x20 | ((size >> 16) & 0x1F)));
        bytes.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(size & 0xFF));
        return;
    }
    bytes.push_back(0x10);
    bytes.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(size & 0xFF));
}

std::vector<std::uint8_t> MatroskaElement(std::uint64_t id, const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes = MatroskaId(id);
    AppendMatroskaSize(bytes, payload.size());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> MatroskaTextElement(std::uint64_t id, std::string_view text)
{
    return MatroskaElement(id, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::vector<std::uint8_t> ConcatForFormatMatrix(std::vector<std::uint8_t> a, std::vector<std::uint8_t> b, std::vector<std::uint8_t> c)
{
    a.insert(a.end(), b.begin(), b.end());
    a.insert(a.end(), c.begin(), c.end());
    return a;
}

std::vector<std::uint8_t> MatroskaAttachedFile(std::string_view fileName, std::string_view mediaType, const std::vector<std::uint8_t> &fileData)
{
    return MatroskaElement(kAttachedFileId, ConcatForFormatMatrix(
                                                 MatroskaTextElement(0x466E, fileName),
                                                 MatroskaTextElement(0x4660, mediaType),
                                                 MatroskaElement(0x465C, fileData)));
}

std::size_t MatroskaVintWidth(std::uint64_t value)
{
    for (std::size_t width = 1; width <= 8; ++width)
    {
        if (value < (1ULL << (7 * width)))
        {
            return width;
        }
    }
    return 8;
}

void AppendMatroskaSizeFixed(std::vector<std::uint8_t> &output, std::uint64_t size, std::size_t width)
{
    const std::uint8_t marker = static_cast<std::uint8_t>(0x80 >> (width - 1));
    for (std::size_t i = 0; i < width; ++i)
    {
        const unsigned shift = static_cast<unsigned>(8 * (width - 1 - i));
        std::uint8_t byte = static_cast<std::uint8_t>((size >> shift) & 0xFF);
        if (i == 0)
        {
            byte = static_cast<std::uint8_t>(marker | (byte & static_cast<std::uint8_t>(marker - 1)));
        }
        output.push_back(byte);
    }
}

bool InsertMatroskaAttachments(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &attachmentsElement)
{
    std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.empty())
    {
        return false;
    }

    std::size_t pos = 0;
    while (pos + 2 <= data.size())
    {
        std::uint64_t elementId = 0;
        std::size_t idLength = 0;
        {
            const std::uint8_t lead = data[pos];
            std::uint8_t mask = 0x80;
            while ((lead & mask) == 0)
            {
                mask >>= 1;
                ++idLength;
            }
            idLength += 1;
            if (pos + idLength > data.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < idLength; ++i)
            {
                elementId = (elementId << 8) | data[pos + i];
            }
        }

        std::uint64_t elementSize = 0;
        std::size_t sizeLength = 0;
        {
            const std::uint8_t lead = data[pos + idLength];
            std::uint8_t mask = 0x80;
            while ((lead & mask) == 0)
            {
                mask >>= 1;
                ++sizeLength;
            }
            sizeLength += 1;
            if (pos + idLength + sizeLength > data.size())
            {
                return false;
            }
            elementSize = static_cast<std::uint64_t>(lead & (mask - 1));
            for (std::size_t i = 1; i < sizeLength; ++i)
            {
                elementSize = (elementSize << 8) | data[pos + idLength + i];
            }
        }

        const std::size_t body = pos + idLength + sizeLength;
        if (elementId == kSegmentId)
        {
            std::uint64_t childId = 0;
            std::size_t childIdLength = 0;
            {
                const std::uint8_t lead = data[body];
                std::uint8_t mask = 0x80;
                while ((lead & mask) == 0)
                {
                    mask >>= 1;
                    ++childIdLength;
                }
                childIdLength += 1;
                for (std::size_t i = 0; i < childIdLength; ++i)
                {
                    childId = (childId << 8) | data[body + i];
                }
            }
            std::uint64_t childSize = 0;
            std::size_t childSizeLength = 0;
            {
                const std::uint8_t lead = data[body + childIdLength];
                std::uint8_t mask = 0x80;
                while ((lead & mask) == 0)
                {
                    mask >>= 1;
                    ++childSizeLength;
                }
                childSizeLength += 1;
                childSize = static_cast<std::uint64_t>(lead & (mask - 1));
                for (std::size_t i = 1; i < childSizeLength; ++i)
                {
                    childSize = (childSize << 8) | data[body + childIdLength + i];
                }
            }
            const std::size_t insertAt = body + childIdLength + childSizeLength + static_cast<std::size_t>(childSize);
            const std::uint64_t newSize = elementSize + attachmentsElement.size();

            std::vector<std::uint8_t> output(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(pos + idLength));
            AppendMatroskaSizeFixed(output, newSize, sizeLength);
            output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(pos + idLength + sizeLength),
                          data.begin() + static_cast<std::ptrdiff_t>(insertAt));
            output.insert(output.end(), attachmentsElement.begin(), attachmentsElement.end());
            output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(insertAt), data.end());
            return WriteBinaryFile(outputPath, output);
        }

        if (body + elementSize > data.size())
        {
            return false;
        }
        pos = body + static_cast<std::size_t>(elementSize);
    }
    return false;
}

bool GenerateWithFfmpeg(const std::filesystem::path &path, const std::string &codecArgs)
{
    const std::string command = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 0.2 " + codecArgs + " \"" + path.string() + "\"";
    return RunFfmpeg(command);
}
} // namespace

bool GenerateFlacAudioSample(const std::filesystem::path &path)
{
    return GenerateWithFfmpeg(path, "-codec:a flac");
}

// 不是所有发行版的 ffmpeg 都带 libvorbis（如 Homebrew 的 ffmpeg 9 未启用）。
// 样本只需要是合法的 Ogg Vorbis 容器，音质无关，因此缺失时退回 FFmpeg
// 自带的原生 vorbis 编码器（标记为 experimental 需 -strict -2，且只支持立体声，
// 故一并 -ac 2）。
static const char *VorbisCodecArgs()
{
    static const std::string args = CommandSucceeds(
                                        "ffmpeg -hide_banner -loglevel error -encoders 2>/dev/null | grep -q ' libvorbis '")
                                        ? "-codec:a libvorbis"
                                        : "-strict -2 -ac 2 -codec:a vorbis";
    return args.c_str();
}

bool GenerateOggVorbisAudioSample(const std::filesystem::path &path)
{
    return GenerateWithFfmpeg(path, VorbisCodecArgs());
}

bool GenerateOpusAudioSample(const std::filesystem::path &path)
{
    return GenerateWithFfmpeg(path, "-ar 48000 -codec:a libopus");
}

bool GenerateMp4AudioSample(const std::filesystem::path &path)
{
    return GenerateWithFfmpeg(path, "-codec:a aac");
}

bool GenerateMatroskaAudioSample(const std::filesystem::path &path)
{
    return GenerateWithFfmpeg(path, "-ar 48000 -codec:a libopus -f matroska");
}

bool GenerateAsfAudioSample(const std::filesystem::path &path)
{
    return GenerateWithFfmpeg(path, "-codec:a wmav2 -f asf");
}

bool GenerateWavAudioSample(const std::filesystem::path &path)
{
    return GenerateWithFfmpeg(path, "-codec:a pcm_s16le");
}

bool GenerateFlacCoverSample(const std::filesystem::path &path)
{
    const std::filesystem::path basePath = path.parent_path() / "base-format-matrix.flac";
    if (!GenerateFlacAudioSample(basePath))
    {
        return false;
    }
    return InjectFlacPictureBlock(basePath, path, FlacPicturePayload(OneByOnePng()));
}

bool GenerateOggVorbisCoverSample(const std::filesystem::path &path)
{
    const std::filesystem::path basePath = path.parent_path() / "base-format-matrix.ogg";
    if (!GenerateOggVorbisAudioSample(basePath))
    {
        return false;
    }
    const std::vector<std::string> comments{"TITLE=format-matrix-ogg", PictureCommentEntry(OneByOnePng())};
    return RebuildOggCommentPage(basePath, path, "\x03vorbis", VorbisCommentPacket(comments));
}

bool GenerateOpusCoverSample(const std::filesystem::path &path)
{
    const std::filesystem::path basePath = path.parent_path() / "base-format-matrix.opus";
    if (!GenerateOpusAudioSample(basePath))
    {
        return false;
    }
    const std::vector<std::string> comments{"TITLE=format-matrix-opus", PictureCommentEntry(OneByOnePng())};
    return RebuildOggCommentPage(basePath, path, "OpusTags", OpusTagsPacket(comments));
}

bool GenerateMp4CoverSample(const std::filesystem::path &path)
{
    const std::filesystem::path basePath = path.parent_path() / "base-format-matrix.m4a";
    if (!GenerateMp4AudioSample(basePath))
    {
        return false;
    }
    return InjectMp4Ilst(basePath, path, Mp4CoverItem(OneByOnePng()));
}

bool GenerateApeCoverSample(const std::filesystem::path &path)
{
    const std::filesystem::path basePath = path.parent_path() / "base-format-matrix.mp3";
    if (!GenerateBaseMp3(basePath))
    {
        return false;
    }
    std::vector<std::uint8_t> audioBytes = ReadBinaryFile(basePath);
    if (audioBytes.empty())
    {
        return false;
    }
    std::vector<std::uint8_t> coverValue{0, 0};
    const std::vector<std::uint8_t> png = OneByOnePng();
    coverValue.insert(coverValue.end(), png.begin(), png.end());
    std::vector<ApeItem> items;
    items.push_back(ApeItem{"Title", std::vector<std::uint8_t>{'f', 'o', 'r', 'm', 'a', 't', '-', 'm', 'a', 't', 'r', 'i', 'x'}, false});
    items.push_back(ApeItem{"Cover Art (Front)", coverValue, true});
    const std::vector<std::uint8_t> tag = ApeTag(items);
    audioBytes.insert(audioBytes.end(), tag.begin(), tag.end());
    return WriteBinaryFile(path, audioBytes);
}

bool GenerateWavId3CoverSample(const std::filesystem::path &path)
{
    const std::filesystem::path basePath = path.parent_path() / "base-format-matrix.wav";
    if (!GenerateWavAudioSample(basePath))
    {
        return false;
    }
    const std::vector<std::uint8_t> tag = Id3v23Tag(Id3v23Frame("APIC", ApicPayload(OneByOnePng())));
    return AppendRiffId3Chunk(basePath, path, tag);
}

bool GenerateAsfCoverSample(const std::filesystem::path &path)
{
    const std::filesystem::path basePath = path.parent_path() / "base-format-matrix.wma";
    if (!GenerateAsfAudioSample(basePath))
    {
        return false;
    }
    return InjectAsfPictureDescriptor(basePath, path, OneByOnePng());
}

bool GenerateMatroskaCoverSample(const std::filesystem::path &path)
{
    const std::filesystem::path basePath = path.parent_path() / "base-format-matrix.mka";
    if (!GenerateMatroskaAudioSample(basePath))
    {
        return false;
    }
    const std::vector<std::uint8_t> attachments = MatroskaElement(
        kAttachmentsId, MatroskaAttachedFile("cover.png", "image/png", OneByOnePng()));
    return InsertMatroskaAttachments(basePath, path, attachments);
}
} // namespace tagreader_test_support
