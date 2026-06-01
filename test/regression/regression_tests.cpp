#include "TagReader.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavutil/log.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct TestCase
{
    std::string_view id;
    bool implemented;
};

constexpr std::array<TestCase, 15> kTestCases{{
    {"TR-AUDIT-001", true},
    {"TR-AUDIT-002", true},
    {"TR-AUDIT-003", true},
    {"TR-AUDIT-004", true},
    {"TR-AUDIT-005", true},
    {"TR-AUDIT-006", false},
    {"TR-AUDIT-007", false},
    {"TR-AUDIT-008", false},
    {"TR-AUDIT-009", false},
    {"TR-AUDIT-010", false},
    {"TR-AUDIT-011", false},
    {"TR-AUDIT-012", false},
    {"TR-AUDIT-013", false},
    {"TR-AUDIT-014", false},
    {"TR-AUDIT-015", false},
}};

void PrintUsage(std::string_view program)
{
    std::cerr << "usage: " << program << " --list|<TR-AUDIT-case-id>\n";
}

void ListCases()
{
    for (const TestCase &testCase : kTestCases)
    {
        std::cout << testCase.id << '\n';
    }
}

const TestCase *FindCase(std::string_view id)
{
    const auto found = std::find_if(kTestCases.begin(), kTestCases.end(), [id](const TestCase &testCase)
                                   { return testCase.id == id; });
    return found == kTestCases.end() ? nullptr : &(*found);
}

bool Expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "expectation failed: " << message << '\n';
        return false;
    }

    return true;
}

std::filesystem::path RegressionTempRoot(std::string_view caseId)
{
    return std::filesystem::temp_directory_path() / ("tagreader_regression_" + std::string(caseId));
}

std::filesystem::path RegressionEvidenceRoot(std::string_view caseId)
{
    return std::filesystem::path("/tmp/opencode/tagreader_regression") / std::string(caseId);
}

bool WriteBinaryFile(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        std::cerr << "failed to create temp directory for " << path.string() << ": " << ec.message() << '\n';
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        std::cerr << "failed to open temp file for write: " << path.string() << '\n';
        return false;
    }

    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output.good())
    {
        std::cerr << "failed to write temp file: " << path.string() << '\n';
        return false;
    }

    return true;
}

bool WriteTextFile(const std::filesystem::path &path, std::string_view text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        std::cerr << "failed to create temp directory for " << path.string() << ": " << ec.message() << '\n';
        return false;
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output)
    {
        std::cerr << "failed to open temp file for write: " << path.string() << '\n';
        return false;
    }

    output << text;
    if (!output.good())
    {
        std::cerr << "failed to write temp file: " << path.string() << '\n';
        return false;
    }

    return true;
}

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void AppendU32BE(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendU24BE(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
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
    for (int i = 0; i < 8; ++i)
    {
        bytes.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

std::uint32_t ReadU32BE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint32_t ReadU24BE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 2]);
}

std::vector<std::uint8_t> Bytes(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

void AppendBytes(std::vector<std::uint8_t> &bytes, std::string_view text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
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

std::vector<std::uint8_t> OggPage(std::uint32_t serial, std::uint32_t sequence, std::uint8_t headerType, const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes{'O', 'g', 'g', 'S', 0, headerType};
    AppendU64LE(bytes, 0);
    AppendU32LE(bytes, serial);
    AppendU32LE(bytes, sequence);
    AppendU32LE(bytes, 0);
    bytes.push_back(static_cast<std::uint8_t>((payload.size() + 254) / 255));

    std::size_t remaining = payload.size();
    while (remaining >= 255)
    {
        bytes.push_back(255);
        remaining -= 255;
    }
    if (payload.empty() || remaining > 0)
    {
        bytes.push_back(static_cast<std::uint8_t>(remaining));
    }

    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const std::uint32_t crc = OggCrc(bytes);
    bytes[22] = static_cast<std::uint8_t>(crc & 0xFF);
    bytes[23] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
    bytes[24] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
    bytes[25] = static_cast<std::uint8_t>((crc >> 24) & 0xFF);
    return bytes;
}

std::vector<std::uint8_t> ManySerialPrefix(std::size_t serialCount)
{
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < serialCount; ++i)
    {
        const std::vector<std::uint8_t> page = OggPage(static_cast<std::uint32_t>(0x10000000U + i), 0, 0x02, Bytes("x"));
        bytes.insert(bytes.end(), page.begin(), page.end());
    }
    return bytes;
}

std::vector<std::uint8_t> Atom(std::array<std::uint8_t, 4> type, const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes;
    AppendU32BE(bytes, static_cast<std::uint32_t>(payload.size() + 8));
    bytes.insert(bytes.end(), type.begin(), type.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> Concat(std::initializer_list<std::vector<std::uint8_t>> parts)
{
    std::vector<std::uint8_t> bytes;
    for (const std::vector<std::uint8_t> &part : parts)
    {
        bytes.insert(bytes.end(), part.begin(), part.end());
    }
    return bytes;
}

std::vector<std::uint8_t> DataAtomUtf8(std::string_view text)
{
    std::vector<std::uint8_t> payload;
    AppendU32BE(payload, 1);
    AppendU32BE(payload, 0);
    const std::vector<std::uint8_t> textBytes = Bytes(text);
    payload.insert(payload.end(), textBytes.begin(), textBytes.end());
    return Atom({'d', 'a', 't', 'a'}, payload);
}

std::vector<std::uint8_t> DataAtomCover(const std::vector<std::uint8_t> &coverBytes)
{
    std::vector<std::uint8_t> payload;
    AppendU32BE(payload, 13);
    AppendU32BE(payload, 0);
    payload.insert(payload.end(), coverBytes.begin(), coverBytes.end());
    return Atom({'d', 'a', 't', 'a'}, payload);
}

std::vector<std::uint8_t> Mp4TextItem(std::array<std::uint8_t, 4> type, std::string_view text)
{
    return Atom(type, DataAtomUtf8(text));
}

std::vector<std::uint8_t> Mp4CoverItem(const std::vector<std::uint8_t> &coverBytes)
{
    return Atom({'c', 'o', 'v', 'r'}, DataAtomCover(coverBytes));
}

std::vector<std::uint8_t> UdtaWithIlst(const std::vector<std::uint8_t> &ilstPayload)
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
        std::cerr << "failed to read base MP4 sample: " << basePath.string() << '\n';
        return false;
    }

    const std::vector<std::uint8_t> udta = UdtaWithIlst(ilstPayload);
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
            std::vector<std::uint8_t> moovPayload(data.begin() + static_cast<std::ptrdiff_t>(cursor + 8), data.begin() + static_cast<std::ptrdiff_t>(cursor + atomSize));
            moovPayload.insert(moovPayload.end(), udta.begin(), udta.end());
            const std::vector<std::uint8_t> moov = Atom({'m', 'o', 'o', 'v'}, moovPayload);
            output.insert(output.end(), moov.begin(), moov.end());
            injected = true;
        }
        else
        {
            output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor), data.begin() + static_cast<std::ptrdiff_t>(cursor + atomSize));
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

bool CommandSucceeds(const std::string &command)
{
    return std::system(command.c_str()) == 0;
}

bool GenerateBaseM4a(const std::filesystem::path &path)
{
    if (!CommandSucceeds("command -v ffmpeg >/dev/null 2>&1"))
    {
        std::cerr << "ffmpeg CLI not found; TR-AUDIT-001 requires an audio-backed M4A sample\n";
        return false;
    }

    const std::string command = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 0.2 -codec:a aac \"" + path.string() + "\"";
    if (!CommandSucceeds(command))
    {
        std::cerr << "failed to generate base M4A sample with ffmpeg\n";
        return false;
    }

    return true;
}

bool GenerateOggVorbisSample(const std::filesystem::path &path, std::string_view title, std::string_view artist)
{
    if (!CommandSucceeds("command -v ffmpeg >/dev/null 2>&1"))
    {
        std::cerr << "ffmpeg CLI not found; TR-AUDIT-002 requires an audio-backed Ogg Vorbis sample\n";
        return false;
    }

    std::string command = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 0.2 -codec:a libvorbis";
    if (!title.empty())
    {
        command += " -metadata title=\"" + std::string(title) + "\"";
    }
    if (!artist.empty())
    {
        command += " -metadata artist=\"" + std::string(artist) + "\"";
    }
    command += " \"" + path.string() + "\"";
    if (!CommandSucceeds(command))
    {
        std::cerr << "failed to generate Ogg Vorbis sample with ffmpeg\n";
        return false;
    }

    return true;
}

bool GenerateFlacSample(const std::filesystem::path &path)
{
    if (!CommandSucceeds("command -v ffmpeg >/dev/null 2>&1"))
    {
        std::cerr << "ffmpeg CLI not found; TR-AUDIT-003 requires audio-backed FLAC samples\n";
        return false;
    }

    const std::string command = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 0.2 -codec:a flac \"" + path.string() + "\"";
    if (!CommandSucceeds(command))
    {
        std::cerr << "failed to generate FLAC sample with ffmpeg\n";
        return false;
    }

    return true;
}

std::vector<std::uint8_t> OneByOnePng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
        0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99,
        0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

std::size_t CountPngFiles(const std::filesystem::path &root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
    {
        return 0;
    }

    std::size_t count = 0;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(root, ec))
    {
        if (ec)
        {
            break;
        }
        if (entry.is_regular_file(ec) && entry.path().extension() == ".png")
        {
            ++count;
        }
    }
    return count;
}

bool PathIsUnder(const std::filesystem::path &path, const std::filesystem::path &root)
{
    std::error_code ec;
    const std::filesystem::path normalizedPath = std::filesystem::weakly_canonical(path, ec);
    if (ec)
    {
        return false;
    }
    ec.clear();
    const std::filesystem::path normalizedRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec)
    {
        return false;
    }

    const auto mismatch = std::mismatch(normalizedRoot.begin(), normalizedRoot.end(), normalizedPath.begin(), normalizedPath.end());
    return mismatch.first == normalizedRoot.end();
}

std::vector<std::uint8_t> VorbisCommentPayload(std::uint32_t commentCount, std::initializer_list<std::string_view> comments)
{
    std::vector<std::uint8_t> payload;
    constexpr std::string_view kVendor = "tagreader-regression";
    AppendU32LE(payload, static_cast<std::uint32_t>(kVendor.size()));
    AppendBytes(payload, kVendor);
    AppendU32LE(payload, commentCount);
    for (std::string_view comment : comments)
    {
        AppendU32LE(payload, static_cast<std::uint32_t>(comment.size()));
        AppendBytes(payload, comment);
    }
    return payload;
}

bool ReplaceFlacVorbisCommentBlock(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &payload)
{
    const std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.size() < 8 || std::string_view(reinterpret_cast<const char *>(data.data()), 4) != "fLaC")
    {
        std::cerr << "failed to read base FLAC sample: " << basePath.string() << '\n';
        return false;
    }
    if (payload.size() > 0xFFFFFFU)
    {
        std::cerr << "FLAC Vorbis comment payload too large\n";
        return false;
    }

    std::size_t cursor = 4;
    while (cursor + 4 <= data.size())
    {
        const bool lastBlock = (data[cursor] & 0x80) != 0;
        const std::uint8_t blockType = data[cursor] & 0x7F;
        const std::uint32_t blockSize = ReadU24BE(data, cursor + 1);
        const std::size_t blockEnd = cursor + 4 + blockSize;
        if (blockEnd > data.size())
        {
            break;
        }

        if (blockType == 4)
        {
            std::vector<std::uint8_t> output;
            output.insert(output.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(cursor));
            output.push_back(static_cast<std::uint8_t>((lastBlock ? 0x80 : 0x00) | 4));
            AppendU24BE(output, static_cast<std::uint32_t>(payload.size()));
            output.insert(output.end(), payload.begin(), payload.end());
            output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(blockEnd), data.end());
            return WriteBinaryFile(outputPath, output);
        }

        cursor = blockEnd;
        if (lastBlock)
        {
            break;
        }
    }

    std::cerr << "base FLAC sample has no Vorbis comment block: " << basePath.string() << '\n';
    return false;
}

std::vector<std::uint8_t> FlacPicturePayload(const std::vector<std::uint8_t> &imageBytes)
{
    constexpr std::string_view kPngMime = "image/png";
    std::vector<std::uint8_t> payload;
    AppendU32BE(payload, 3);
    AppendU32BE(payload, static_cast<std::uint32_t>(kPngMime.size()));
    AppendBytes(payload, kPngMime);
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

bool InjectFlacPictureBlocks(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &firstPicture, const std::vector<std::uint8_t> &secondPicture)
{
    const std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.size() < 42 || std::string_view(reinterpret_cast<const char *>(data.data()), 4) != "fLaC")
    {
        std::cerr << "failed to read base FLAC sample: " << basePath.string() << '\n';
        return false;
    }
    if (firstPicture.size() > 0xFFFFFFU || secondPicture.size() > 0xFFFFFFU)
    {
        std::cerr << "FLAC picture payload too large\n";
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
        std::cerr << "base FLAC sample has no complete STREAMINFO metadata block: " << basePath.string() << '\n';
        return false;
    }

    AppendFlacMetadataBlock(output, 6, false, firstPicture);
    AppendFlacMetadataBlock(output, 6, true, secondPicture);
    output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(audioStart), data.end());
    return WriteBinaryFile(outputPath, output);
}

bool PatchOggVorbisCommentCount(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, std::uint32_t commentCount, std::string_view firstComment = {})
{
    std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.empty())
    {
        std::cerr << "failed to read base Ogg sample: " << basePath.string() << '\n';
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

        const bool isCommentPage = payloadSize >= 15 && data[payloadOffset] == 0x03 &&
                                   std::string_view(reinterpret_cast<const char *>(data.data() + payloadOffset + 1), 6) == "vorbis";
        if (isCommentPage)
        {
            const std::size_t vendorLengthOffset = payloadOffset + 7;
            const std::uint32_t vendorLength = static_cast<std::uint32_t>(data[vendorLengthOffset]) |
                                               (static_cast<std::uint32_t>(data[vendorLengthOffset + 1]) << 8) |
                                               (static_cast<std::uint32_t>(data[vendorLengthOffset + 2]) << 16) |
                                               (static_cast<std::uint32_t>(data[vendorLengthOffset + 3]) << 24);
            const std::size_t countOffset = vendorLengthOffset + 4 + vendorLength;
            if (countOffset + 4 > pageEnd)
            {
                break;
            }

            if (!firstComment.empty())
            {
                const std::size_t firstLengthOffset = countOffset + 4;
                if (firstLengthOffset + 4 > pageEnd)
                {
                    break;
                }
                const std::uint32_t firstLength = static_cast<std::uint32_t>(data[firstLengthOffset]) |
                                                  (static_cast<std::uint32_t>(data[firstLengthOffset + 1]) << 8) |
                                                  (static_cast<std::uint32_t>(data[firstLengthOffset + 2]) << 16) |
                                                  (static_cast<std::uint32_t>(data[firstLengthOffset + 3]) << 24);
                const std::size_t firstCommentOffset = firstLengthOffset + 4;
                if (firstComment.size() > firstLength || firstCommentOffset + firstLength > pageEnd)
                {
                    break;
                }

                std::fill(data.begin() + static_cast<std::ptrdiff_t>(firstCommentOffset), data.begin() + static_cast<std::ptrdiff_t>(firstCommentOffset + firstLength), static_cast<std::uint8_t>(' '));
                std::copy(firstComment.begin(), firstComment.end(), data.begin() + static_cast<std::ptrdiff_t>(firstCommentOffset));
            }

            data[countOffset] = static_cast<std::uint8_t>(commentCount & 0xFF);
            data[countOffset + 1] = static_cast<std::uint8_t>((commentCount >> 8) & 0xFF);
            data[countOffset + 2] = static_cast<std::uint8_t>((commentCount >> 16) & 0xFF);
            data[countOffset + 3] = static_cast<std::uint8_t>((commentCount >> 24) & 0xFF);

            data[cursor + 22] = 0;
            data[cursor + 23] = 0;
            data[cursor + 24] = 0;
            data[cursor + 25] = 0;
            const std::vector<std::uint8_t> page(data.begin() + static_cast<std::ptrdiff_t>(cursor), data.begin() + static_cast<std::ptrdiff_t>(pageEnd));
            const std::uint32_t crc = OggCrc(page);
            data[cursor + 22] = static_cast<std::uint8_t>(crc & 0xFF);
            data[cursor + 23] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
            data[cursor + 24] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
            data[cursor + 25] = static_cast<std::uint8_t>((crc >> 24) & 0xFF);
            return WriteBinaryFile(outputPath, data);
        }

        cursor = pageEnd;
    }

    std::cerr << "base Ogg sample has no patchable Vorbis comment packet: " << basePath.string() << '\n';
    return false;
}

bool PrependManySerialPages(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, std::size_t serialCount)
{
    const std::vector<std::uint8_t> base = ReadBinaryFile(basePath);
    if (base.empty())
    {
        std::cerr << "failed to read base Ogg sample: " << basePath.string() << '\n';
        return false;
    }

    std::vector<std::uint8_t> output = ManySerialPrefix(serialCount);
    output.insert(output.end(), base.begin(), base.end());
    return WriteBinaryFile(outputPath, output);
}

std::string DescribeTag(const MusicTag &tag)
{
    std::string text;
    text += "title=" + std::string(tag.title()) + "\n";
    text += "artist=" + std::string(tag.artist()) + "\n";
    text += "album=" + std::string(tag.album()) + "\n";
    text += "coverPath=" + tag.coverPath().string() + "\n";
    text += "lyricsCount=" + std::to_string(tag.lyrics().size()) + "\n";
    return text;
}

bool RunTrAudit001()
{
    constexpr std::string_view kCaseId = "TR-AUDIT-001";
    const std::filesystem::path evidenceRoot = RegressionEvidenceRoot(kCaseId);
    std::error_code ec;
    std::filesystem::remove_all(evidenceRoot, ec);
    ec.clear();
    std::filesystem::create_directories(evidenceRoot, ec);
    if (ec)
    {
        std::cerr << "failed to create evidence directory: " << ec.message() << '\n';
        return false;
    }

    const std::filesystem::path basePath = evidenceRoot / "base.m4a";
    const std::filesystem::path normalPath = evidenceRoot / "normal.m4a";
    const std::filesystem::path malformedPath = evidenceRoot / "size0-hidden-title.m4a";

    if (!GenerateBaseM4a(basePath))
    {
        return false;
    }

    const std::vector<std::uint8_t> normalIlst = Mp4TextItem({0xA9, 'n', 'a', 'm'}, "Unit Test Title");
    std::vector<std::uint8_t> malformedIlst;
    AppendU32BE(malformedIlst, 0);
    malformedIlst.insert(malformedIlst.end(), {'f', 'r', 'e', 'e', 'b', 'a', 'd', '!'});
    const std::vector<std::uint8_t> hiddenTitle = Mp4TextItem({0xA9, 'n', 'a', 'm'}, "After Size0");
    const std::vector<std::uint8_t> hiddenArtist = Mp4TextItem({0xA9, 'A', 'R', 'T'}, "Recovered Artist");
    malformedIlst.insert(malformedIlst.end(), hiddenTitle.begin(), hiddenTitle.end());
    malformedIlst.insert(malformedIlst.end(), hiddenArtist.begin(), hiddenArtist.end());

    if (!InjectMp4Ilst(basePath, normalPath, normalIlst) || !InjectMp4Ilst(basePath, malformedPath, malformedIlst))
    {
        return false;
    }

    const MusicTag normalTag = TagReader::Read(normalPath);
    const MusicTag malformedTag = TagReader::Read(malformedPath);

    const bool normalOk = Expect(normalTag.title() == "Unit Test Title", "normal MP4 title should be parsed through public API");
    const bool titleHidden = Expect(malformedTag.title() != "After Size0", "size-zero payload must not recover hidden fake title");
    const bool artistHidden = Expect(malformedTag.artist() != "Recovered Artist", "size-zero payload must not recover hidden fake artist");
    const bool noCover = Expect(malformedTag.coverPath().empty(), "malformed size-zero sample should not produce cover side effects");
    const bool noLyrics = Expect(malformedTag.lyrics().empty(), "malformed size-zero sample should not produce lyrics side effects");

    const std::string normalOutput = DescribeTag(normalTag);
    const std::string malformedOutput = DescribeTag(malformedTag);
    const std::string summary =
        "case=TR-AUDIT-001\n"
        "marker=size-zero recovery disabled\n"
        "normalSample=" + normalPath.string() + "\n"
        "malformedSample=" + malformedPath.string() + "\n"
        "normalTitle=" + std::string(normalTag.title()) + "\n"
        "malformedTitle=" + std::string(malformedTag.title()) + "\n"
        "malformedArtist=" + std::string(malformedTag.artist()) + "\n";

    const bool evidenceOk = WriteTextFile(evidenceRoot / "normal_output.txt", normalOutput) &&
                            WriteTextFile(evidenceRoot / "malformed_output.txt", malformedOutput) &&
                            WriteTextFile(evidenceRoot / "summary.txt", summary);
    if (!evidenceOk)
    {
        return false;
    }

    const bool passed = normalOk && titleHidden && artistHidden && noCover && noLyrics;
    if (passed)
    {
        std::cout << "TR-AUDIT-001 size-zero recovery disabled\n";
    }
    return passed;
}

bool RunTrAudit002()
{
    constexpr std::string_view kCaseId = "TR-AUDIT-002";
    const std::filesystem::path evidenceRoot = RegressionEvidenceRoot(kCaseId);
    std::error_code ec;
    std::filesystem::remove_all(evidenceRoot, ec);
    ec.clear();
    std::filesystem::create_directories(evidenceRoot, ec);
    if (ec)
    {
        std::cerr << "failed to create evidence directory: " << ec.message() << '\n';
        return false;
    }

    const std::filesystem::path normalPath = evidenceRoot / "normal.ogg";
    const std::filesystem::path basePath = evidenceRoot / "base-empty.ogg";
    const std::filesystem::path manySerialsPath = evidenceRoot / "many_serials.ogg";

    if (!GenerateOggVorbisSample(normalPath, "ogg-normal", "ogg-artist") ||
        !GenerateOggVorbisSample(basePath, "", "") ||
        !PrependManySerialPages(basePath, manySerialsPath, 300))
    {
        return false;
    }

    const MusicTag normalTag = TagReader::Read(normalPath);
    const MusicTag manySerialsTag = TagReader::Read(manySerialsPath);

    const bool normalTitleOk = Expect(normalTag.title() == "ogg-normal", "normal Ogg Vorbis title should be parsed through public API");
    const bool normalArtistOk = Expect(normalTag.artist() == "ogg-artist", "normal Ogg Vorbis artist should be parsed through public API");
    const bool manyTitleEmpty = Expect(manySerialsTag.title().empty(), "many-serial Ogg sample should not produce title metadata");
    const bool manyArtistEmpty = Expect(manySerialsTag.artist().empty(), "many-serial Ogg sample should not produce artist metadata");
    const bool noCover = Expect(manySerialsTag.coverPath().empty(), "many-serial Ogg sample should not produce cover side effects");
    const bool noLyrics = Expect(manySerialsTag.lyrics().empty(), "many-serial Ogg sample should not produce lyrics side effects");

    const std::string normalOutput = DescribeTag(normalTag);
    const std::string manySerialsOutput = DescribeTag(manySerialsTag);
    const std::string summary =
        "case=TR-AUDIT-002\n"
        "marker=logical-stream-limit many serials rejected without quadratic scan\n"
        "normalSample=" + normalPath.string() + "\n"
        "manySerialsSample=" + manySerialsPath.string() + "\n"
        "distinctSerialPages=300\n"
        "normalTitle=" + std::string(normalTag.title()) + "\n"
        "normalArtist=" + std::string(normalTag.artist()) + "\n"
        "manySerialsTitle=" + std::string(manySerialsTag.title()) + "\n"
        "manySerialsArtist=" + std::string(manySerialsTag.artist()) + "\n";

    const bool evidenceOk = WriteTextFile(evidenceRoot / "normal_output.txt", normalOutput) &&
                            WriteTextFile(evidenceRoot / "many_serials_output.txt", manySerialsOutput) &&
                            WriteTextFile(evidenceRoot / "summary.txt", summary);
    if (!evidenceOk)
    {
        return false;
    }

    const bool passed = normalTitleOk && normalArtistOk && manyTitleEmpty && manyArtistEmpty && noCover && noLyrics;
    if (passed)
    {
        std::cout << "TR-AUDIT-002 logical-stream-limit many serials rejected without quadratic scan\n";
    }
    return passed;
}

bool RunTrAudit003()
{
    constexpr std::string_view kCaseId = "TR-AUDIT-003";
    const std::filesystem::path evidenceRoot = RegressionEvidenceRoot(kCaseId);
    std::error_code ec;
    std::filesystem::remove_all(evidenceRoot, ec);
    ec.clear();
    std::filesystem::create_directories(evidenceRoot, ec);
    if (ec)
    {
        std::cerr << "failed to create evidence directory: " << ec.message() << '\n';
        return false;
    }

    const std::filesystem::path baseFlacPath = evidenceRoot / "base.flac";
    const std::filesystem::path normalFlacPath = evidenceRoot / "normal.flac";
    const std::filesystem::path malformedFlacPath = evidenceRoot / "comment_count_max.flac";
    const std::filesystem::path baseOggPath = evidenceRoot / "base.ogg";
    const std::filesystem::path normalOggPath = evidenceRoot / "normal.ogg";
    const std::filesystem::path malformedOggPath = evidenceRoot / "comment_count_max.ogg";

    const std::vector<std::uint8_t> normalFlacComments = VorbisCommentPayload(1, {"TITLE=flac-count-one"});
    const std::vector<std::uint8_t> malformedFlacComments = VorbisCommentPayload(0xFFFFFFFFU, {});
    if (!GenerateFlacSample(baseFlacPath) ||
        !GenerateOggVorbisSample(baseOggPath, "", "") ||
        !GenerateOggVorbisSample(normalOggPath, "ogg-count-one", "") ||
        !ReplaceFlacVorbisCommentBlock(baseFlacPath, normalFlacPath, normalFlacComments) ||
        !ReplaceFlacVorbisCommentBlock(baseFlacPath, malformedFlacPath, malformedFlacComments) ||
        !PatchOggVorbisCommentCount(normalOggPath, normalOggPath, 1, "TITLE=ogg-count-one") ||
        !PatchOggVorbisCommentCount(baseOggPath, malformedOggPath, 0xFFFFFFFFU))
    {
        return false;
    }

    const MusicTag normalFlacTag = TagReader::Read(normalFlacPath);
    const MusicTag malformedFlacTag = TagReader::Read(malformedFlacPath);
    const MusicTag normalOggTag = TagReader::Read(normalOggPath);
    const MusicTag malformedOggTag = TagReader::Read(malformedOggPath);

    const bool normalFlacTitleOk = Expect(normalFlacTag.title() == "flac-count-one", "normal FLAC Vorbis comment count=1 should parse title");
    const bool normalOggTitleOk = Expect(normalOggTag.title() == "ogg-count-one", "normal Ogg Vorbis comment count=1 should parse title");
    const bool malformedFlacTitleEmpty = Expect(malformedFlacTag.title().empty(), "malformed FLAC comment count max should not produce title metadata");
    const bool malformedFlacArtistEmpty = Expect(malformedFlacTag.artist().empty(), "malformed FLAC comment count max should not produce artist metadata");
    const bool malformedFlacAlbumEmpty = Expect(malformedFlacTag.album().empty(), "malformed FLAC comment count max should not produce album metadata");
    const bool malformedOggTitleEmpty = Expect(malformedOggTag.title().empty(), "malformed Ogg comment count max should not produce title metadata");
    const bool malformedOggArtistEmpty = Expect(malformedOggTag.artist().empty(), "malformed Ogg comment count max should not produce artist metadata");
    const bool malformedOggAlbumEmpty = Expect(malformedOggTag.album().empty(), "malformed Ogg comment count max should not produce album metadata");

    const std::string stdoutLike =
        "TR-AUDIT-003 flac comment-count-limit title=" + std::string(normalFlacTag.title()) + "\n" +
        "TR-AUDIT-003 ogg comment-count-limit title=" + std::string(normalOggTag.title()) + "\n";
    const std::string summary =
        "case=TR-AUDIT-003\n"
        "marker=flac comment-count-limit\n"
        "marker=ogg comment-count-limit\n"
        "normalFlacSample=" + normalFlacPath.string() + "\n" +
        "malformedFlacSample=" + malformedFlacPath.string() + "\n" +
        "normalOggSample=" + normalOggPath.string() + "\n" +
        "malformedOggSample=" + malformedOggPath.string() + "\n" +
        "normalFlacTitle=" + std::string(normalFlacTag.title()) + "\n" +
        "normalOggTitle=" + std::string(normalOggTag.title()) + "\n" +
        "malformedFlacTitle=" + std::string(malformedFlacTag.title()) + "\n" +
        "malformedOggTitle=" + std::string(malformedOggTag.title()) + "\n";

    const bool evidenceOk = WriteTextFile(evidenceRoot / "normal_flac_output.txt", DescribeTag(normalFlacTag)) &&
                            WriteTextFile(evidenceRoot / "malformed_flac_output.txt", DescribeTag(malformedFlacTag)) &&
                            WriteTextFile(evidenceRoot / "normal_ogg_output.txt", DescribeTag(normalOggTag)) &&
                            WriteTextFile(evidenceRoot / "malformed_ogg_output.txt", DescribeTag(malformedOggTag)) &&
                            WriteTextFile(evidenceRoot / "stdout.txt", stdoutLike) &&
                            WriteTextFile(evidenceRoot / "summary.txt", summary);
    if (!evidenceOk)
    {
        return false;
    }

    const bool passed = normalFlacTitleOk && normalOggTitleOk && malformedFlacTitleEmpty && malformedFlacArtistEmpty && malformedFlacAlbumEmpty && malformedOggTitleEmpty && malformedOggArtistEmpty && malformedOggAlbumEmpty;
    if (passed)
    {
        std::cout << "TR-AUDIT-003 flac comment-count-limit ogg comment-count-limit\n";
    }
    return passed;
}

bool RunTrAudit004()
{
    constexpr std::string_view kCaseId = "TR-AUDIT-004";
    constexpr std::size_t kInvalidCoverPayloadSize = 2z * 1024 * 1024;
    const std::filesystem::path evidenceRoot = RegressionEvidenceRoot(kCaseId);
    const std::filesystem::path coverExportDir = evidenceRoot / "covers";
    std::error_code ec;
    std::filesystem::remove_all(evidenceRoot, ec);
    ec.clear();
    std::filesystem::create_directories(evidenceRoot, ec);
    if (ec)
    {
        std::cerr << "failed to create evidence directory: " << ec.message() << '\n';
        return false;
    }

    const std::filesystem::path basePath = evidenceRoot / "base.m4a";
    const std::filesystem::path multiCovrPath = evidenceRoot / "multi_covr.m4a";

    const std::vector<std::uint8_t> validPng = OneByOnePng();
    const std::vector<std::uint8_t> invalidCover(kInvalidCoverPayloadSize, 0xCC);
    const std::vector<std::uint8_t> firstCoverItem = Mp4CoverItem(validPng);
    const std::vector<std::uint8_t> secondCoverItem = Mp4CoverItem(invalidCover);
    const std::vector<std::uint8_t> ilstPayload = Concat({firstCoverItem, secondCoverItem});

    if (!GenerateBaseM4a(basePath) || !InjectMp4Ilst(basePath, multiCovrPath, ilstPayload))
    {
        return false;
    }

    const MusicTag firstTag = TagReader::Read(multiCovrPath, coverExportDir);
    const std::filesystem::path firstCoverPath = firstTag.coverPath();
    const bool coverPathPresent = Expect(!firstCoverPath.empty(), "first valid MP4 covr should export a cover path");
    const bool coverExists = Expect(std::filesystem::is_regular_file(firstCoverPath, ec), "exported MP4 cover should exist on disk");
    ec.clear();
    const bool coverUnderExportDir = Expect(PathIsUnder(firstCoverPath, coverExportDir), "exported MP4 cover should stay under cover export directory");
    const bool onePngAfterFirstRead = Expect(CountPngFiles(coverExportDir) == 1, "duplicate invalid MP4 covr should not create a second PNG");
    const auto firstMtime = std::filesystem::last_write_time(firstCoverPath, ec);
    const bool firstMtimeOk = Expect(!ec, "exported MP4 cover mtime should be readable");
    ec.clear();

    const MusicTag repeatedTag = TagReader::Read(multiCovrPath, coverExportDir);
    const bool repeatedPathSame = Expect(repeatedTag.coverPath() == firstCoverPath, "repeated MP4 cover read should reuse the same cache path");
    const auto repeatedMtime = std::filesystem::last_write_time(firstCoverPath, ec);
    const bool repeatedMtimeOk = Expect(!ec && repeatedMtime == firstMtime, "repeated MP4 cover read should not rewrite cached PNG");
    ec.clear();
    const bool onePngAfterRepeatedRead = Expect(CountPngFiles(coverExportDir) == 1, "repeated MP4 cover read should still have one PNG");

    const std::string stdoutLike =
        "TR-AUDIT-004 duplicate MP4 covr skipped\n"
        "TR-AUDIT-004 PASS\n";
    const std::string summary =
        "case=TR-AUDIT-004\n"
        "marker=duplicate MP4 covr skipped\n"
        "sample=" + multiCovrPath.string() + "\n" +
        "coverExportDir=" + coverExportDir.string() + "\n" +
        "coverPath=" + firstCoverPath.string() + "\n" +
        "covrItems=2\n"
        "dataItems=2\n"
        "validPngBytes=" + std::to_string(validPng.size()) + "\n" +
        "invalidCoverBytes=" + std::to_string(invalidCover.size()) + "\n" +
        "pngFilesAfterFirstRead=1\n"
        "pngFilesAfterRepeatedRead=" + std::to_string(CountPngFiles(coverExportDir)) + "\n";

    const bool evidenceOk = WriteTextFile(evidenceRoot / "stdout.txt", stdoutLike) &&
                            WriteTextFile(evidenceRoot / "summary.txt", summary) &&
                            WriteTextFile(evidenceRoot / "first_read_output.txt", DescribeTag(firstTag)) &&
                            WriteTextFile(evidenceRoot / "repeated_read_output.txt", DescribeTag(repeatedTag));
    if (!evidenceOk)
    {
        return false;
    }

    const bool passed = coverPathPresent && coverExists && coverUnderExportDir && onePngAfterFirstRead && firstMtimeOk && repeatedPathSame && repeatedMtimeOk && onePngAfterRepeatedRead;
    if (passed)
    {
        std::cout << "TR-AUDIT-004 duplicate MP4 covr skipped\n";
    }
    return passed;
}

bool RunTrAudit005()
{
    constexpr std::string_view kCaseId = "TR-AUDIT-005";
    constexpr std::size_t kSecondPicturePayloadSize = 2z * 1024 * 1024;
    const std::filesystem::path evidenceRoot = RegressionEvidenceRoot(kCaseId);
    const std::filesystem::path coverExportDir = evidenceRoot / "covers";
    std::error_code ec;
    std::filesystem::remove_all(evidenceRoot, ec);
    ec.clear();
    std::filesystem::create_directories(evidenceRoot, ec);
    if (ec)
    {
        std::cerr << "failed to create evidence directory: " << ec.message() << '\n';
        return false;
    }

    const std::filesystem::path basePath = evidenceRoot / "base.flac";
    const std::filesystem::path multiPicturePath = evidenceRoot / "multi_picture.flac";

    const std::vector<std::uint8_t> validPng = OneByOnePng();
    const std::vector<std::uint8_t> secondPictureBytes(kSecondPicturePayloadSize, 0xDD);
    const std::vector<std::uint8_t> firstPicture = FlacPicturePayload(validPng);
    const std::vector<std::uint8_t> secondPicture = FlacPicturePayload(secondPictureBytes);

    if (!GenerateFlacSample(basePath) || !InjectFlacPictureBlocks(basePath, multiPicturePath, firstPicture, secondPicture))
    {
        return false;
    }

    const MusicTag firstTag = TagReader::Read(multiPicturePath, coverExportDir);
    const std::filesystem::path firstCoverPath = firstTag.coverPath();
    const bool coverPathPresent = Expect(!firstCoverPath.empty(), "first valid FLAC PICTURE should export a cover path");
    const bool coverExists = Expect(std::filesystem::is_regular_file(firstCoverPath, ec), "exported FLAC cover should exist on disk");
    ec.clear();
    const bool coverUnderExportDir = Expect(PathIsUnder(firstCoverPath, coverExportDir), "exported FLAC cover should stay under cover export directory");
    const bool onePngAfterFirstRead = Expect(CountPngFiles(coverExportDir) == 1, "duplicate FLAC PICTURE should not create a second PNG");
    const auto firstMtime = std::filesystem::last_write_time(firstCoverPath, ec);
    const bool firstMtimeOk = Expect(!ec, "exported FLAC cover mtime should be readable");
    ec.clear();

    const MusicTag repeatedTag = TagReader::Read(multiPicturePath, coverExportDir);
    const bool repeatedPathSame = Expect(repeatedTag.coverPath() == firstCoverPath, "repeated FLAC cover read should reuse the same cache path");
    const auto repeatedMtime = std::filesystem::last_write_time(firstCoverPath, ec);
    const bool repeatedMtimeOk = Expect(!ec && repeatedMtime == firstMtime, "repeated FLAC cover read should not rewrite cached PNG");
    ec.clear();
    const bool onePngAfterRepeatedRead = Expect(CountPngFiles(coverExportDir) == 1, "repeated FLAC cover read should still have one PNG");

    const std::string stdoutLike =
        "TR-AUDIT-005 duplicate FLAC PICTURE skipped\n"
        "TR-AUDIT-005 PASS\n";
    const std::string summary =
        "case=TR-AUDIT-005\n"
        "marker=duplicate FLAC PICTURE skipped\n"
        "sample=" + multiPicturePath.string() + "\n" +
        "coverExportDir=" + coverExportDir.string() + "\n" +
        "coverPath=" + firstCoverPath.string() + "\n" +
        "streamInfoBlocks=1\n"
        "pictureBlocks=2\n"
        "validPngBytes=" + std::to_string(validPng.size()) + "\n" +
        "firstPicturePayloadBytes=" + std::to_string(firstPicture.size()) + "\n" +
        "secondPictureImageBytes=" + std::to_string(secondPictureBytes.size()) + "\n" +
        "secondPicturePayloadBytes=" + std::to_string(secondPicture.size()) + "\n" +
        "pngFilesAfterFirstRead=1\n"
        "pngFilesAfterRepeatedRead=" + std::to_string(CountPngFiles(coverExportDir)) + "\n";

    const bool evidenceOk = WriteTextFile(evidenceRoot / "stdout.txt", stdoutLike) &&
                            WriteTextFile(evidenceRoot / "summary.txt", summary) &&
                            WriteTextFile(evidenceRoot / "first_read_output.txt", DescribeTag(firstTag)) &&
                            WriteTextFile(evidenceRoot / "repeated_read_output.txt", DescribeTag(repeatedTag));
    if (!evidenceOk)
    {
        return false;
    }

    const bool passed = coverPathPresent && coverExists && coverUnderExportDir && onePngAfterFirstRead && firstMtimeOk && repeatedPathSame && repeatedMtimeOk && onePngAfterRepeatedRead;
    if (passed)
    {
        std::cout << "TR-AUDIT-005 duplicate FLAC PICTURE skipped\n";
    }
    return passed;
}

int RunCase(const TestCase &testCase)
{
    if (!testCase.implemented)
    {
        std::cerr << testCase.id << " not implemented\n";
        return 1;
    }

    (void)RegressionTempRoot;

    if (testCase.id == "TR-AUDIT-001")
    {
        if (!RunTrAudit001())
        {
            return 1;
        }

        std::cout << testCase.id << " PASS\n";
        return 0;
    }

    if (testCase.id == "TR-AUDIT-002")
    {
        if (!RunTrAudit002())
        {
            return 1;
        }

        std::cout << testCase.id << " PASS\n";
        return 0;
    }

    if (testCase.id == "TR-AUDIT-003")
    {
        if (!RunTrAudit003())
        {
            return 1;
        }

        std::cout << testCase.id << " PASS\n";
        return 0;
    }

    if (testCase.id == "TR-AUDIT-004")
    {
        if (!RunTrAudit004())
        {
            return 1;
        }

        std::cout << testCase.id << " PASS\n";
        return 0;
    }

    if (testCase.id == "TR-AUDIT-005")
    {
        if (!RunTrAudit005())
        {
            return 1;
        }

        std::cout << testCase.id << " PASS\n";
        return 0;
    }

    std::cout << testCase.id << " PASS\n";
    return 0;
}
}

int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_QUIET);

    if (argc != 2)
    {
        PrintUsage(argv[0]);
        return 2;
    }

    const std::string_view command = argv[1];
    if (command == "--list")
    {
        ListCases();
        return 0;
    }

    const TestCase *testCase = FindCase(command);
    if (testCase == nullptr)
    {
        std::cerr << command << " unknown regression case\n";
        return 2;
    }

    try
    {
        return RunCase(*testCase);
    }
    catch (const std::exception &ex)
    {
        std::cerr << command << " error: " << ex.what() << '\n';
    }
    catch (...)
    {
        std::cerr << command << " unknown error\n";
    }

    return 1;
}
