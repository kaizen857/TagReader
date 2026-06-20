#!/usr/bin/env python3
"""Generate minimal TagReader security smoke samples.

All generated files are written under /tmp/opencode/tagreader_security_samples.
The script intentionally creates both normal audio-backed samples and malformed
parser-target samples used by later implementation phases.
"""

from __future__ import annotations

import argparse
import base64
import shutil
import struct
import subprocess
import sys
from pathlib import Path


DEFAULT_OUT_DIR = Path("/tmp/opencode/tagreader_security_samples")

PNG_1X1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
)

ASF_HEADER_GUID = bytes.fromhex("3026b2758e66cf11a6d900aa0062ce6c")
ASF_EXTENDED_CONTENT_DESCRIPTION_GUID = bytes.fromhex("40a4d0d207e3d21197f000a0c95ea850")


def syncsafe32(value: int) -> bytes:
    if value < 0 or value > 0x0FFFFFFF:
        raise ValueError(f"value out of syncsafe range: {value}")
    return bytes(
        [
            (value >> 21) & 0x7F,
            (value >> 14) & 0x7F,
            (value >> 7) & 0x7F,
            value & 0x7F,
        ]
    )


def flac_picture_block(image_bytes: bytes, mime: bytes = b"image/png", description: bytes = b"cover") -> bytes:
    return (
        struct.pack(">I", 3)
        + struct.pack(">I", len(mime))
        + mime
        + struct.pack(">I", len(description))
        + description
        + struct.pack(">IIII", 1, 1, 24, 0)
        + struct.pack(">I", len(image_bytes))
        + image_bytes
    )


def metadata_block_picture_value(image_bytes: bytes = PNG_1X1) -> str:
    return base64.b64encode(flac_picture_block(image_bytes)).decode("ascii")


def utf16le_text(text: str, terminated: bool = True) -> bytes:
    encoded = text.encode("utf-16-le")
    return encoded + (b"\x00\x00" if terminated else b"")


def asf_object(guid: bytes, payload: bytes) -> bytes:
    return guid + struct.pack("<Q", len(payload) + 24) + payload


def asf_extended_descriptor(name: str, value_type: int, value: bytes) -> bytes:
    name_bytes = utf16le_text(name)
    return struct.pack("<H", len(name_bytes)) + name_bytes + struct.pack("<HH", value_type, len(value)) + value


def asf_extended_content_description(descriptors: list[bytes]) -> bytes:
    return asf_object(ASF_EXTENDED_CONTENT_DESCRIPTION_GUID, struct.pack("<H", len(descriptors)) + b"".join(descriptors))


def asf_picture_value(image_bytes: bytes) -> bytes:
    return b"\x03" + struct.pack("<I", len(image_bytes)) + utf16le_text("image/png") + utf16le_text("front cover") + image_bytes


def id3v22_frame(frame_id: str, payload: bytes) -> bytes:
    if len(frame_id) != 3:
        raise ValueError("ID3v2.2 frame id must be 3 bytes")
    return frame_id.encode("ascii") + len(payload).to_bytes(3, "big") + payload


def id3v22_tag(frames: list[bytes], flags: int = 0) -> bytes:
    body = b"".join(frames)
    return b"ID3" + bytes([2, 0, flags]) + syncsafe32(len(body)) + body


def id3v24_frame(frame_id: str, payload: bytes, flags: bytes = b"\x00\x00") -> bytes:
    if len(frame_id) != 4:
        raise ValueError("ID3v2.4 frame id must be 4 bytes")
    return frame_id.encode("ascii") + syncsafe32(len(payload)) + flags + payload


def id3v24_tag(frames: list[bytes]) -> bytes:
    body = b"".join(frames)
    return b"ID3" + bytes([4, 0, 0]) + syncsafe32(len(body)) + body


def make_text_frame(frame_id: str, text: str) -> bytes:
    return id3v24_frame(frame_id, b"\x03" + text.encode("utf-8"))


def make_uslt_frame(text: str) -> bytes:
    payload = b"\x03eng\x00" + text.encode("utf-8")
    return id3v24_frame("USLT", payload)


def make_ult_frame(text: str) -> bytes:
    payload = b"\x03eng\x00" + text.encode("utf-8")
    return id3v22_frame("ULT", payload)


def make_apic_frame(image_bytes: bytes, mime: str = "image/png") -> bytes:
    payload = b"\x03" + mime.encode("ascii") + b"\x00" + b"\x03" + b"cover\x00" + image_bytes
    return id3v24_frame("APIC", payload)


def run_ffmpeg(args: list[str]) -> bool:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        print("warning: ffmpeg CLI not found; audio-backed samples will be skipped", file=sys.stderr)
        return False

    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *args]
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        print(f"warning: ffmpeg failed: {' '.join(command)}", file=sys.stderr)
        return False
    return True


def generate_base_mp3(path: Path) -> bool:
    return run_ffmpeg(
        [
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=44100:cl=mono",
            "-t",
            "0.2",
            "-codec:a",
            "libmp3lame",
            str(path),
        ]
    )


def generate_base_m4a(path: Path) -> bool:
    return run_ffmpeg(
        [
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=44100:cl=mono",
            "-t",
            "0.2",
            "-codec:a",
            "aac",
            str(path),
        ]
    )


def generate_base_ogg(path: Path) -> bool:
    return run_ffmpeg(
        [
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=44100:cl=mono",
            "-t",
            "0.2",
            "-codec:a",
            "libvorbis",
            str(path),
        ]
    )


def generate_metadata_ogg(path: Path) -> bool:
    return run_ffmpeg(
        [
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=44100:cl=mono",
            "-t",
            "0.2",
            "-codec:a",
            "libvorbis",
            "-metadata",
            "title=Ogg Title",
            "-metadata",
            "artist=Ogg Artist",
            "-metadata",
            "album=Ogg Album",
            "-metadata",
            "album_artist=Ogg Album Artist",
            "-metadata",
            "composer=Ogg Composer",
            "-metadata",
            "genre=Ogg Genre",
            "-metadata",
            "date=2026",
            "-metadata",
            "tracknumber=3",
            "-metadata",
            "discnumber=1",
            "-metadata",
            "lyrics=Ogg lyric line",
            str(path),
        ]
    )


def generate_metadata_opus(path: Path) -> bool:
    return run_ffmpeg(
        [
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=48000:cl=mono",
            "-t",
            "0.2",
            "-codec:a",
            "libopus",
            "-metadata",
            "title=Opus Security Title",
            str(path),
        ]
    )


def generate_ogg_picture(path: Path) -> bool:
    return run_ffmpeg(
        [
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=44100:cl=mono",
            "-t",
            "0.2",
            "-codec:a",
            "libvorbis",
            "-metadata",
            "title=Ogg Picture Security",
            "-metadata",
            f"METADATA_BLOCK_PICTURE={metadata_block_picture_value()}",
            str(path),
        ]
    )


def generate_opus_picture(path: Path) -> bool:
    return run_ffmpeg(
        [
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=48000:cl=mono",
            "-t",
            "0.2",
            "-codec:a",
            "libopus",
            "-metadata",
            "title=Opus Picture Security",
            "-metadata",
            f"METADATA_BLOCK_PICTURE={metadata_block_picture_value()}",
            str(path),
        ]
    )


def write_id3v24_apic(path: Path, image_bytes: bytes, base_mp3: Path) -> None:
    frames = [
        make_text_frame("TIT2", "Security APIC Sample"),
        make_apic_frame(image_bytes),
    ]
    path.write_bytes(id3v24_tag(frames) + base_mp3.read_bytes())


def write_malformed_noncover_metadata(path: Path, base_mp3: Path) -> None:
    bad_utf8_title = id3v24_frame("TIT2", b"\x03\xff\xfe\xfa")
    truncated_artist = b"TPE1" + syncsafe32(32) + b"\x00\x00\x03bad"
    frames = [bad_utf8_title, truncated_artist]
    path.write_bytes(id3v24_tag(frames) + base_mp3.read_bytes())


def write_lrc_id3(path: Path, text: str, base_mp3: Path) -> None:
    frames = [
        make_text_frame("TIT2", "Security LRC Sample"),
        make_uslt_frame(text),
    ]
    path.write_bytes(id3v24_tag(frames) + base_mp3.read_bytes())


def write_id3v22_lyrics(path: Path, text: str, flags: int, base_mp3: Path) -> None:
    path.write_bytes(id3v22_tag([make_ult_frame(text)], flags) + base_mp3.read_bytes())


def atom(atom_type: bytes, payload: bytes) -> bytes:
    if len(atom_type) != 4:
        raise ValueError("MP4 atom type must be 4 bytes")
    return struct.pack(">I4s", len(payload) + 8, atom_type) + payload


def data_atom_utf8(text: str) -> bytes:
    payload = struct.pack(">II", 1, 0) + text.encode("utf-8")
    return atom(b"data", payload)


def data_atom(data_type: int, payload: bytes) -> bytes:
    return atom(b"data", struct.pack(">II", data_type, 0) + payload)


def utf16be_bom_text(text: str) -> bytes:
    return b"\xfe\xff" + text.encode("utf-16-be")


def inject_mp4_ilst(base_m4a: Path, path: Path, ilst_payload: bytes) -> None:
    data = base_m4a.read_bytes()
    output = bytearray()
    cursor = 0
    injected = False
    udta = atom(b"udta", atom(b"meta", b"\x00\x00\x00\x00" + atom(b"ilst", ilst_payload)))

    while cursor + 8 <= len(data):
        size, atom_type = struct.unpack(">I4s", data[cursor : cursor + 8])
        if size < 8 or cursor + size > len(data):
            break
        payload = data[cursor + 8 : cursor + size]
        if atom_type == b"moov" and not injected:
            output += atom(b"moov", payload + udta)
            injected = True
        else:
            output += data[cursor : cursor + size]
        cursor += size

    output += data[cursor:]
    if not injected:
        output += atom(b"moov", udta)
    path.write_bytes(bytes(output))


def write_deep_mp4(path: Path, depth: int) -> None:
    payload = atom(b"free", b"deep")
    for _ in range(depth):
        payload = atom(b"moov", payload)
    path.write_bytes(atom(b"ftyp", b"M4A \x00\x00\x00\x00M4A ") + payload)


def write_mp4_lyrics_atom(path: Path) -> None:
    lyr = atom("\xa9lyr".encode("latin-1"), data_atom_utf8("[00:01.000]mp4 lyric"))
    ilst = atom(b"ilst", lyr)
    meta = atom(b"meta", b"\x00\x00\x00\x00" + ilst)
    udta = atom(b"udta", meta)
    moov = atom(b"moov", udta)
    path.write_bytes(atom(b"ftyp", b"M4A \x00\x00\x00\x00M4A ") + moov)


def write_mp4_lyrics_sample(path: Path, base_m4a: Path, data_type: int, payload: bytes) -> None:
    lyr = atom("\xa9lyr".encode("latin-1"), data_atom(data_type, payload))
    inject_mp4_ilst(base_m4a, path, lyr)


def write_mp4_size0_tail_ok(path: Path, base_m4a: Path) -> None:
    ilst_payload = (
        atom(b"\xa9nam", data_atom_utf8("Size0 Tail OK"))
        + atom(b"\xa9ART", data_atom_utf8("Size0 Artist"))
        + struct.pack(">I4s", 0, b"free")
    )
    inject_mp4_ilst(base_m4a, path, ilst_payload)


def write_mp4_size0_hides_metadata(path: Path, base_m4a: Path) -> None:
    hidden_sibling = (
        struct.pack(">I4s", 0, b"free")
        + b"bad!"
        + atom(b"\xa9nam", data_atom_utf8("After Size0"))
        + atom(b"\xa9ART", data_atom_utf8("Recovered Artist"))
    )
    inject_mp4_ilst(base_m4a, path, hidden_sibling)


def ogg_page(serial: int, sequence: int, header_type: int, segment_sizes: list[int], payload: bytes) -> bytes:
    header = bytearray()
    header += b"OggS"
    header += b"\x00"
    header += bytes([header_type])
    header += struct.pack("<Q", 0)
    header += struct.pack("<I", serial)
    header += struct.pack("<I", sequence)
    header += struct.pack("<I", 0)
    header += bytes([len(segment_sizes)])
    header += bytes(segment_sizes)
    return bytes(header) + payload


def vorbis_block(entries: list[bytes], vendor: bytes = b"tagreader") -> bytes:
    payload = struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", len(entries))
    for entry in entries:
        payload += struct.pack("<I", len(entry)) + entry
    return payload


def ogg_segments(size: int) -> list[int]:
    segments = [255] * (size // 255)
    segments.append(size % 255)
    return segments


def prepend_ogg_page(path: Path, page: bytes) -> None:
    path.write_bytes(page + path.read_bytes())


def write_non_vorbis_or_video_mixed(path: Path, base_ogg: Path) -> None:
    theora_ident = b"\x80theora" + b"\x00" * 32
    path.write_bytes(ogg_page(0x0A0B0C0D, 0, 0x02, [len(theora_ident)], theora_ident) + base_ogg.read_bytes())


def write_ogg_comment_resource_limit(path: Path) -> None:
    serial = 0x13572468
    ident = b"\x01vorbis" + bytes(23)
    output = bytearray(ogg_page(serial, 0, 0x02, [len(ident)], ident))
    for sequence in range(1, 512):
        header_type = 0x01 if sequence > 1 else 0x00
        output += ogg_page(serial, sequence, header_type, [255], b"R" * 255)
    path.write_bytes(bytes(output))


def write_ogg_vorbis_music_multistream(path: Path) -> bool:
    if not generate_metadata_ogg(path):
        return False

    opus_head = b"OpusHead" + b"\x01\x01" + b"\x00" * 16
    prepend_ogg_page(path, ogg_page(0x0BADF00D, 0, 0x02, [len(opus_head)], opus_head))
    return True


def write_ogg_continuation(path: Path, pages: int) -> None:
    serial = 0x12345678
    output = bytearray()
    for sequence in range(pages):
        header_type = 0x01 if sequence else 0x00
        output += ogg_page(serial, sequence, header_type, [255], b"A" * 255)
    path.write_bytes(bytes(output))


def write_id3v24_large_declared(path: Path, base_mp3: Path, declared_size: int) -> None:
    header = b"ID3" + bytes([4, 0, 0]) + syncsafe32(declared_size)
    path.write_bytes(header + base_mp3.read_bytes())


def inject_asf_header_children(base_asf: Path, path: Path, children: list[bytes]) -> bool:
    data = base_asf.read_bytes()
    header_index = data.find(ASF_HEADER_GUID)
    if header_index < 0 or header_index + 30 > len(data):
        return False
    declared_size = struct.unpack("<Q", data[header_index + 16 : header_index + 24])[0]
    if declared_size < 30 or header_index + declared_size > len(data):
        return False
    child_count = struct.unpack("<I", data[header_index + 24 : header_index + 28])[0]
    payload = data[header_index + 30 : header_index + declared_size] + b"".join(children)
    replacement = ASF_HEADER_GUID + struct.pack("<Q", len(payload) + 30) + struct.pack("<I", child_count + len(children)) + data[header_index + 28 : header_index + 30] + payload
    path.write_bytes(data[:header_index] + replacement + data[header_index + declared_size :])
    return True


def write_asf_picture_sample(path: Path, base_asf: Path) -> bool:
    descriptor = asf_extended_descriptor("WM/Picture", 1, asf_picture_value(PNG_1X1))
    title = asf_extended_descriptor("Title", 0, utf16le_text("ASF Picture Security"))
    return inject_asf_header_children(base_asf, path, [asf_extended_content_description([title, descriptor])])


def matroska_id(element_id: int) -> bytes:
    length = max(1, (element_id.bit_length() + 7) // 8)
    return element_id.to_bytes(length, "big")


def matroska_size(size: int) -> bytes:
    if size <= 0x7F:
        return bytes([0x80 | size])
    if size <= 0x3FFF:
        return bytes([0x40 | ((size >> 8) & 0x3F), size & 0xFF])
    if size <= 0x1FFFFF:
        return bytes([0x20 | ((size >> 16) & 0x1F), (size >> 8) & 0xFF, size & 0xFF])
    if size <= 0x0FFFFFFF:
        return bytes([0x10 | ((size >> 24) & 0x0F), (size >> 16) & 0xFF, (size >> 8) & 0xFF, size & 0xFF])
    return bytes([0x08 | ((size >> 32) & 0x07), (size >> 24) & 0xFF, (size >> 16) & 0xFF, (size >> 8) & 0xFF, size & 0xFF])


def matroska_element(element_id: int, payload: bytes) -> bytes:
    return matroska_id(element_id) + matroska_size(len(payload)) + payload


def matroska_text_element(element_id: int, text: str) -> bytes:
    return matroska_element(element_id, text.encode("utf-8"))


def matroska_simple_tag(name: str, value: str) -> bytes:
    return matroska_element(0x67C8, matroska_text_element(0x45A3, name) + matroska_text_element(0x4487, value))


def matroska_attached_file(file_name: str, media_type: str, file_data: bytes) -> bytes:
    return matroska_element(
        0x61A7,
        matroska_text_element(0x466E, file_name)
        + matroska_text_element(0x4660, media_type)
        + matroska_element(0x465C, file_data),
    )


def matroska_file(segment_payload: bytes) -> bytes:
    ebml = matroska_element(0x1A45DFA3, matroska_element(0x4286, b"\x01") + matroska_text_element(0x4282, "matroska"))
    return ebml + matroska_element(0x18538067, segment_payload)


def write_matroska_picture_fixture(path: Path) -> None:
    tags = matroska_element(0x1254C367, matroska_element(0x7373, matroska_simple_tag("TITLE", "Matroska Picture Security")))
    attachments = matroska_element(0x1941A469, matroska_attached_file("cover.png", "image/png", PNG_1X1))
    path.write_bytes(matroska_file(tags + attachments))


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate minimal TagReader security smoke samples")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    args = parser.parse_args()
    out_dir = args.out_dir

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    smoke_samples: list[Path] = []
    parser_level_samples: list[Path] = []
    skips: list[str] = []

    base_mp3 = out_dir / "base.mp3"
    if generate_base_mp3(base_mp3):
        id3_apic = out_dir / "id3v24_apic_png.mp3"
        cover_cache = out_dir / "cover_cache_base.mp3"
        cover_export = out_dir / "cover_export_base.mp3"
        malformed = out_dir / "malformed_noncover_metadata.mp3"
        write_id3v24_apic(id3_apic, PNG_1X1, base_mp3)
        write_id3v24_apic(cover_cache, PNG_1X1, base_mp3)
        write_id3v24_apic(cover_export, PNG_1X1, base_mp3)
        write_malformed_noncover_metadata(malformed, base_mp3)
        smoke_samples.extend([id3_apic, cover_cache, cover_export, malformed])
        write_lrc_id3(
            out_dir / "id3v24_uslt_lrc.mp3",
            "[00:01.000]first line\n[00:02.000][00:03.000]repeated line",
            base_mp3,
        )
        write_lrc_id3(out_dir / "id3v24_uslt_invalid_lrc.mp3", "[abc:def]bad timestamp", base_mp3)
        write_lrc_id3(
            out_dir / "lyrics_bracket_plain.mp3",
            "[ar:Unit Test Artist]\n[Verse]\n[hello]\n[Chorus] sing",
            base_mp3,
        )
        write_lrc_id3(
            out_dir / "lyrics_timed_multi.mp3",
            "[00:01.00]first timed line\n[00:02.00]second timed line",
            base_mp3,
        )
        write_id3v22_lyrics(
            out_dir / "id3v22_lyrics_flagged.mp3",
            "ID3v22 flagged lyric line",
            0x80,
            base_mp3,
        )
        write_id3v22_lyrics(
            out_dir / "id3v22_lyrics_unsupported_flag.mp3",
            "ID3v22 unsupported damaged lyric payload",
            0x40,
            base_mp3,
        )
        write_id3v24_large_declared(out_dir / "id3v24_declared_32m.mp3", base_mp3, 32 * 1024 * 1024)
    else:
        skips.append("mp3/id3 cover smoke samples skipped: ffmpeg mp3 generation failed or codec unavailable")

    base_m4a = out_dir / "base.m4a"
    if generate_base_m4a(base_m4a):
        write_mp4_lyrics_sample(
            out_dir / "mp4_lyrics_utf16_bom.m4a",
            base_m4a,
            2,
            utf16be_bom_text("MP4 UTF16 lyric line"),
        )
        write_mp4_lyrics_sample(
            out_dir / "mp4_lyrics_utf8.m4a",
            base_m4a,
            1,
            b"MP4 UTF8 lyric line",
        )
        write_mp4_lyrics_sample(
            out_dir / "mp4_lyrics_oversized.m4a",
            base_m4a,
            1,
            b"O" * (8 * 1024 * 1024 + 1),
        )
        write_mp4_size0_tail_ok(out_dir / "mp4_size0_tail_ok.m4a", base_m4a)
        write_mp4_size0_hides_metadata(out_dir / "mp4_size0_hides_metadata.m4a", base_m4a)
    else:
        skips.append("mp4 lyrics smoke samples skipped: ffmpeg m4a generation failed or codec unavailable")

    base_ogg = out_dir / "base.ogg"
    if generate_base_ogg(base_ogg):
        write_ogg_vorbis_music_multistream(out_dir / "ogg_vorbis_music_multistream_comments.ogg")
        write_non_vorbis_or_video_mixed(out_dir / "ogg_non_vorbis_or_video_mixed.ogg", base_ogg)
        ogg_picture = out_dir / "ogg_vorbis_picture_cover.ogg"
        if generate_ogg_picture(ogg_picture):
            smoke_samples.append(ogg_picture)
        else:
            skips.append("ogg/vorbis picture smoke sample skipped: ffmpeg picture metadata generation failed")
    else:
        skips.append("ogg/vorbis cover smoke sample skipped: ffmpeg ogg generation failed or codec unavailable")

    opus_picture = out_dir / "ogg_opus_picture_cover.opus"
    if generate_opus_picture(opus_picture):
        smoke_samples.append(opus_picture)
    else:
        skips.append("ogg/opus picture smoke sample skipped: ffmpeg opus picture metadata generation failed")

    base_asf = out_dir / "base.wma"
    asf_picture = out_dir / "asf_picture_cover.wma"
    if run_ffmpeg(["-f", "lavfi", "-i", "anullsrc=r=44100:cl=mono", "-t", "0.2", "-codec:a", "wmav2", str(base_asf)]):
        if write_asf_picture_sample(asf_picture, base_asf):
            smoke_samples.append(asf_picture)
        else:
            parser_level_samples.append(asf_picture)
            skips.append("asf picture smoke sample documented as parser-level: could not inject ASF header metadata into ffmpeg output")
    else:
        synthetic_asf = out_dir / "asf_picture_cover.parser-fixture.wma"
        synthetic_asf.write_bytes(asf_object(ASF_HEADER_GUID, struct.pack("<I", 1) + b"\x01\x02" + asf_extended_content_description([asf_extended_descriptor("WM/Picture", 1, asf_picture_value(PNG_1X1))])))
        parser_level_samples.append(synthetic_asf)
        skips.append("asf picture smoke sample documented as parser-level: ffmpeg wma generation failed or codec unavailable")

    matroska_fixture = out_dir / "matroska_attachment_cover.parser-fixture.webm"
    write_matroska_picture_fixture(matroska_fixture)
    parser_level_samples.append(matroska_fixture)
    skips.append("matroska attachment cover documented as parser-level fixture: minimal synthetic EBML has no audio stream for TagReaderSecuritySmoke")

    write_deep_mp4(out_dir / "mp4_deep_nested_atoms.m4a", 128)
    write_mp4_lyrics_atom(out_dir / "mp4_lyrics_atom.m4a")
    write_ogg_continuation(out_dir / "ogg_continuation_packet.ogg", 512)
    write_ogg_comment_resource_limit(out_dir / "ogg_comment_resource_limit.ogg")

    manifest_lines = ["# TagReader security samples", "", "## smoke_samples"]
    manifest_lines.extend(str(path) for path in sorted(smoke_samples))
    manifest_lines.extend(["", "## parser_level_samples"])
    manifest_lines.extend(str(path) for path in sorted(parser_level_samples))
    manifest_lines.extend(["", "## documented_skips"])
    manifest_lines.extend(skips)
    (out_dir / "MANIFEST.txt").write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")

    print(f"generated samples under {out_dir}")
    for skip in skips:
        print(f"warning: {skip}", file=sys.stderr)
    for path in sorted(out_dir.iterdir()):
        if path.is_file():
            print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
