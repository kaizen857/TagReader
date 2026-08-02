#pragma once

#include <filesystem>

namespace tagreader_test_support
{
// Runtime-generated audio bases (ffmpeg), one per container family.
bool GenerateFlacAudioSample(const std::filesystem::path &path);
bool GenerateOggVorbisAudioSample(const std::filesystem::path &path);
bool GenerateOpusAudioSample(const std::filesystem::path &path);
bool GenerateMp4AudioSample(const std::filesystem::path &path);
bool GenerateMatroskaAudioSample(const std::filesystem::path &path);
bool GenerateAsfAudioSample(const std::filesystem::path &path);
bool GenerateWavAudioSample(const std::filesystem::path &path);

// Runtime-generated embedded-cover fixtures, one per supported format.
// None of these ship binaries; every fixture is rebuilt from ffmpeg bases
// plus byte-level tag/picture injection at test time.
bool GenerateFlacCoverSample(const std::filesystem::path &path);
bool GenerateOggVorbisCoverSample(const std::filesystem::path &path);
bool GenerateOpusCoverSample(const std::filesystem::path &path);
bool GenerateMp4CoverSample(const std::filesystem::path &path);
bool GenerateApeCoverSample(const std::filesystem::path &path);
bool GenerateWavId3CoverSample(const std::filesystem::path &path);
bool GenerateAsfCoverSample(const std::filesystem::path &path);
bool GenerateMatroskaCoverSample(const std::filesystem::path &path);
}
