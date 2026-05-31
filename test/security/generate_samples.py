#!/usr/bin/env python3
"""Generate minimal TagReader security smoke samples.

All generated files are written under /tmp/opencode/tagreader_security_samples.
The script intentionally creates both normal audio-backed samples and malformed
parser-target samples used by later implementation phases.
"""

from __future__ import annotations

import base64
import shutil
import struct
import subprocess
import sys
from pathlib import Path


OUT_DIR = Path("/tmp/opencode/tagreader_security_samples")

PNG_1X1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
)


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


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    base_mp3 = OUT_DIR / "base.mp3"
    if generate_base_mp3(base_mp3):
        write_id3v24_apic(OUT_DIR / "id3v24_apic_png.mp3", PNG_1X1, base_mp3)
        write_id3v24_apic(OUT_DIR / "cover_cache_base.mp3", PNG_1X1, base_mp3)
        write_id3v24_apic(OUT_DIR / "cover_export_base.mp3", PNG_1X1, base_mp3)
        write_malformed_noncover_metadata(OUT_DIR / "malformed_noncover_metadata.mp3", base_mp3)
        write_lrc_id3(
            OUT_DIR / "id3v24_uslt_lrc.mp3",
            "[00:01.000]first line\n[00:02.000][00:03.000]repeated line",
            base_mp3,
        )
        write_lrc_id3(OUT_DIR / "id3v24_uslt_invalid_lrc.mp3", "[abc:def]bad timestamp", base_mp3)
        write_lrc_id3(
            OUT_DIR / "lyrics_bracket_plain.mp3",
            "[ar:Unit Test Artist]\n[Verse]\n[hello]\n[Chorus] sing",
            base_mp3,
        )
        write_lrc_id3(
            OUT_DIR / "lyrics_timed_multi.mp3",
            "[00:01.00]first timed line\n[00:02.00]second timed line",
            base_mp3,
        )
        write_id3v22_lyrics(
            OUT_DIR / "id3v22_lyrics_flagged.mp3",
            "ID3v22 flagged lyric line",
            0x80,
            base_mp3,
        )
        write_id3v22_lyrics(
            OUT_DIR / "id3v22_lyrics_unsupported_flag.mp3",
            "ID3v22 unsupported damaged lyric payload",
            0x40,
            base_mp3,
        )
        write_id3v24_large_declared(OUT_DIR / "id3v24_declared_32m.mp3", base_mp3, 32 * 1024 * 1024)

    base_m4a = OUT_DIR / "base.m4a"
    if generate_base_m4a(base_m4a):
        write_mp4_lyrics_sample(
            OUT_DIR / "mp4_lyrics_utf16_bom.m4a",
            base_m4a,
            2,
            utf16be_bom_text("MP4 UTF16 lyric line"),
        )
        write_mp4_lyrics_sample(
            OUT_DIR / "mp4_lyrics_utf8.m4a",
            base_m4a,
            1,
            b"MP4 UTF8 lyric line",
        )
        write_mp4_lyrics_sample(
            OUT_DIR / "mp4_lyrics_oversized.m4a",
            base_m4a,
            1,
            b"O" * (8 * 1024 * 1024 + 1),
        )
        write_mp4_size0_tail_ok(OUT_DIR / "mp4_size0_tail_ok.m4a", base_m4a)
        write_mp4_size0_hides_metadata(OUT_DIR / "mp4_size0_hides_metadata.m4a", base_m4a)
    base_ogg = OUT_DIR / "base.ogg"
    if generate_base_ogg(base_ogg):
        write_ogg_vorbis_music_multistream(OUT_DIR / "ogg_vorbis_music_multistream_comments.ogg")
        write_non_vorbis_or_video_mixed(OUT_DIR / "ogg_non_vorbis_or_video_mixed.ogg", base_ogg)

    write_deep_mp4(OUT_DIR / "mp4_deep_nested_atoms.m4a", 128)
    write_mp4_lyrics_atom(OUT_DIR / "mp4_lyrics_atom.m4a")
    write_ogg_continuation(OUT_DIR / "ogg_continuation_packet.ogg", 512)
    write_ogg_comment_resource_limit(OUT_DIR / "ogg_comment_resource_limit.ogg")

    print(f"generated samples under {OUT_DIR}")
    for path in sorted(OUT_DIR.iterdir()):
        if path.is_file():
            print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
