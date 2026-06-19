# ape-tag-support: Decisions

## Architecture
- DetectedContainer::Ape added to enum; NormalizeContainerFormatName falls through to Unknown path
- APE footer detection in ContainerDetector.cpp precedes ID3 header check (line ~79)
- TagPipeline.cpp ReadMetadata Ape case: APE first, then ID3v2→ID3v1 fallback ONLY for mp3/mpeg containers
- Cover uses WriteCoverAsPng() from tagreader_cover namespace

## Limits
- kMaxApeTagBytes = 16 MiB
- kMaxApeItems = 4096
- kMaxApeItemValueBytes = 1 MiB
