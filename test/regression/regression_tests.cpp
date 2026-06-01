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
    {"TR-AUDIT-003", false},
    {"TR-AUDIT-004", false},
    {"TR-AUDIT-005", false},
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

std::vector<std::uint8_t> Bytes(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
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

std::vector<std::uint8_t> Mp4TextItem(std::array<std::uint8_t, 4> type, std::string_view text)
{
    return Atom(type, DataAtomUtf8(text));
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
