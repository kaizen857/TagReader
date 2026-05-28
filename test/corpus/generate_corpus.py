#!/usr/bin/env python3
"""Generate deterministic fuzz corpus seeds for TagReader.

The script writes generated seeds under /tmp/opencode/tagreader_fuzz_corpus by
default. It does not read parent directories or require external audio tools.
"""

from __future__ import annotations

import argparse
import base64
import shutil
import struct
from pathlib import Path


DEFAULT_OUT_DIR = Path("/tmp/opencode/tagreader_fuzz_corpus")
MAX_TEXT_FIELD_BYTES = 1024 * 1024
MAX_DECODED_TEXT_BYTES = 2 * 1024 * 1024
MAX_PLAIN_LYRICS_BYTES = 1024 * 1024
MAX_LYRIC_LINES = 20000
MAX_LRC_TIMESTAMPS_PER_LINE = 32

PNG_1X1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
)


def syncsafe32(value: int) -> bytes:
    return bytes(
        [
            (value >> 21) & 0x7F,
            (value >> 14) & 0x7F,
            (value >> 7) & 0x7F,
            value & 0x7F,
        ]
    )


def atom(atom_type: bytes, payload: bytes) -> bytes:
    return struct.pack(">I4s", len(payload) + 8, atom_type) + payload


def extended_atom(atom_type: bytes, payload: bytes) -> bytes:
    return struct.pack(">I4sQ", 1, atom_type, len(payload) + 16) + payload


def data_atom(data_type: int, payload: bytes) -> bytes:
    return atom(b"data", struct.pack(">II", data_type, 0) + payload)


def mp4_metadata_file(ilst_payload: bytes, full_box: bytes = b"\0\0\0\0") -> bytes:
    return atom(b"ftyp", b"M4A \0\0\0\0M4A ") + atom(
        b"moov", atom(b"udta", atom(b"meta", full_box + atom(b"ilst", ilst_payload)))
    )


def id3v22_frame(frame_id: bytes, payload: bytes) -> bytes:
    return frame_id + len(payload).to_bytes(3, "big") + payload


def id3v23_frame(frame_id: bytes, payload: bytes, flags: bytes = b"\0\0") -> bytes:
    return frame_id + len(payload).to_bytes(4, "big") + flags + payload


def id3v24_frame(frame_id: bytes, payload: bytes, flags: bytes = b"\0\0") -> bytes:
    return frame_id + syncsafe32(len(payload)) + flags + payload


def id3_tag(version: int, frames: bytes, flags: int = 0) -> bytes:
    return b"ID3" + bytes([version, 0, flags]) + syncsafe32(len(frames)) + frames


def id3v24_footer(frames: bytes, flags: int = 0) -> bytes:
    return b"3DI" + bytes([4, 0, flags]) + syncsafe32(len(frames))


def id3_unsync(payload: bytes) -> bytes:
    result = bytearray()
    for index, value in enumerate(payload):
        result.append(value)
        next_value = payload[index + 1] if index + 1 < len(payload) else None
        if value == 0xFF and (next_value == 0x00 or next_value is None or next_value >= 0xE0):
            result.append(0)
    return bytes(result)


def ogg_page(serial: int, sequence: int, header_type: int, segments: list[int], payload: bytes) -> bytes:
    return (
        b"OggS"
        + b"\0"
        + bytes([header_type])
        + struct.pack("<Q", 0)
        + struct.pack("<I", serial)
        + struct.pack("<I", sequence)
        + struct.pack("<I", 0)
        + bytes([len(segments)])
        + bytes(segments)
        + payload
    )


def vorbis_block(entries: list[bytes], vendor: bytes = b"vendor") -> bytes:
    payload = struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", len(entries))
    for entry in entries:
        payload += struct.pack("<I", len(entry)) + entry
    return payload


def flac_block(block_type: int, payload: bytes, last: bool = False) -> bytes:
    return bytes([(0x80 if last else 0) | (block_type & 0x7F)]) + len(payload).to_bytes(3, "big") + payload


def flac_file(*blocks: bytes) -> bytes:
    return b"fLaC" + b"".join(blocks)


def flac_picture_block(mime: bytes, description: bytes, image: bytes, declared_image_len: int | None = None) -> bytes:
    image_len = len(image) if declared_image_len is None else declared_image_len
    return (
        struct.pack(">I", 3)
        + struct.pack(">I", len(mime))
        + mime
        + struct.pack(">I", len(description))
        + description
        + struct.pack(">IIII", 1, 1, 24, 0)
        + struct.pack(">I", image_len)
        + image
    )


def lrc_line(timestamp_count: int, text: bytes = b"line") -> bytes:
    return b"".join(f"[00:{index % 60:02d}.00]".encode() for index in range(timestamp_count)) + text + b"\n"


def write_seed(out_dir: Path, category: str, name: str, data: bytes) -> None:
    category_dir = out_dir / category
    category_dir.mkdir(parents=True, exist_ok=True)
    (category_dir / name).write_bytes(data)


def generate_id3(out_dir: Path) -> None:
    v22 = id3_tag(2, id3v22_frame(b"TT2", b"\x03v22 title"))
    v23 = id3_tag(3, id3v23_frame(b"TIT2", b"\x03v23 title"))
    v24 = id3_tag(4, id3v24_frame(b"TIT2", b"\x03v24 title"))
    truncated = b"ID3\x04\0\0" + syncsafe32(32) + b"TIT2\0"
    oversized = b"ID3\x04\0\0" + syncsafe32(17 * 1024 * 1024)

    large_unknown_v22 = id3_tag(
        2,
        id3v22_frame(b"GEO", b"G" * (MAX_TEXT_FIELD_BYTES + 256))
        + id3v22_frame(b"TT2", b"\x03after v22 unknown"),
    )
    large_geob_v23 = id3_tag(
        3,
        id3v23_frame(b"GEOB", b"application/octet-stream\0blob\0" + b"G" * (MAX_TEXT_FIELD_BYTES + 256))
        + id3v23_frame(b"TIT2", b"\x03after geob"),
    )
    large_priv_v24 = id3_tag(
        4,
        id3v24_frame(b"PRIV", b"owner@example\0" + b"P" * (MAX_TEXT_FIELD_BYTES + 256))
        + id3v24_frame(b"TIT2", b"\x03after priv"),
    )
    frame_unsync_payload = id3_unsync(b"\x00frame unsync \xff\xe0 title")
    frame_unsync_text_v24 = id3_tag(4, id3v24_frame(b"TIT2", frame_unsync_payload, b"\0\x02"))
    tag_unsync_text_v24 = id3_tag(4, id3v24_frame(b"TIT2", id3_unsync(b"\x00tag unsync \xff\xe0 title")), 0x80)
    tag_and_frame_unsync_v24 = id3_tag(4, id3v24_frame(b"TIT2", frame_unsync_payload, b"\0\x02"), 0x80)
    extended_header = syncsafe32(6) + b"\x01\0"
    tag_unsync_extended_v24 = id3_tag(
        4,
        extended_header + id3v24_frame(b"TIT2", id3_unsync(b"\x00extended \xff\xe0 title")),
        0xC0,
    )
    footer_frames = id3v24_frame(b"TIT2", id3_unsync(b"\x00tag unsync footer \xff\xe0"))
    tag_unsync_footer_v24 = id3_tag(4, footer_frames + id3v24_footer(footer_frames, 0x90), 0x90)
    tag_unsync_apic_v24 = id3_tag(
        4,
        id3v24_frame(b"APIC", b"\x03image/png\0\x03cover\0" + id3_unsync(PNG_1X1)),
        0x80,
    )

    slt_many_lines_payload = b"\x03eng\x02\x01\0" + b"".join(
        b"line\0" + struct.pack(">I", index) for index in range(MAX_LYRIC_LINES + 16)
    )
    sylt_many_lines_payload = b"\x03eng\x02\x01\0" + b"".join(
        b"line\0" + struct.pack(">I", index) for index in range(MAX_LYRIC_LINES + 16)
    )
    lrc_many_timestamps = id3_tag(3, id3v23_frame(b"USLT", b"\x03eng\0" + lrc_line(MAX_LRC_TIMESTAMPS_PER_LINE + 16)))
    lrc_many_lines = id3_tag(
        3,
        id3v23_frame(b"USLT", b"\x03eng\0" + b"".join(lrc_line(1, b"line") for _ in range(MAX_LYRIC_LINES + 16))),
    )
    plain_over_limit = id3_tag(3, id3v23_frame(b"USLT", b"\x03eng\0" + b"P" * (MAX_PLAIN_LYRICS_BYTES + 1)))
    slt_many_lines = id3_tag(2, id3v22_frame(b"SLT", slt_many_lines_payload))
    sylt_many_lines = id3_tag(3, id3v23_frame(b"SYLT", sylt_many_lines_payload))

    write_seed(out_dir, "id3", "id3v22_minimal.mp3", v22)
    write_seed(out_dir, "id3", "id3v23_minimal.mp3", v23)
    write_seed(out_dir, "id3", "id3v24_minimal.mp3", v24)
    write_seed(out_dir, "id3", "id3v24_truncated.mp3", truncated)
    write_seed(out_dir, "id3", "id3v24_oversized.mp3", oversized)
    write_seed(out_dir, "id3", "id3v22_large_unknown_geo.mp3", large_unknown_v22)
    write_seed(out_dir, "id3", "id3v23_large_geob.mp3", large_geob_v23)
    write_seed(out_dir, "id3", "id3v24_large_priv.mp3", large_priv_v24)
    write_seed(out_dir, "id3", "id3v24_frame_unsync_text.mp3", frame_unsync_text_v24)
    write_seed(out_dir, "id3", "id3v24_tag_unsync_text.mp3", tag_unsync_text_v24)
    write_seed(out_dir, "id3", "id3v24_tag_and_frame_unsync_text.mp3", tag_and_frame_unsync_v24)
    write_seed(out_dir, "id3", "id3v24_tag_unsync_extended_header.mp3", tag_unsync_extended_v24)
    write_seed(out_dir, "id3", "id3v24_tag_unsync_footer.mp3", tag_unsync_footer_v24)
    write_seed(out_dir, "id3", "id3v24_tag_unsync_apic.mp3", tag_unsync_apic_v24)
    write_seed(out_dir, "id3", "id3v23_lrc_timestamp_explosion.mp3", lrc_many_timestamps)
    write_seed(out_dir, "id3", "id3v23_lrc_line_cap.mp3", lrc_many_lines)
    write_seed(out_dir, "id3", "id3v23_plain_lyrics_over_limit.mp3", plain_over_limit)
    write_seed(out_dir, "id3", "id3v22_slt_line_cap.mp3", slt_many_lines)
    write_seed(out_dir, "id3", "id3v23_sylt_line_cap.mp3", sylt_many_lines)


def generate_flac(out_dir: Path) -> None:
    streaminfo = bytes(34)
    valid = flac_file(flac_block(0, streaminfo), flac_block(4, vorbis_block([b"TITLE=flac"]), True))
    truncated = b"fLaC" + bytes([0x04]) + (32).to_bytes(3, "big") + b"short"
    oversized = b"fLaC" + bytes([0x04]) + (0xFFFFFF).to_bytes(3, "big") + b"x"
    picture_valid = flac_file(flac_block(0, streaminfo), flac_block(6, flac_picture_block(b"image/png", b"cover", PNG_1X1), True))
    picture_mime_truncated = flac_file(flac_block(0, streaminfo), flac_block(6, struct.pack(">II", 3, 64) + b"image", True))
    picture_desc_truncated = flac_file(
        flac_block(0, streaminfo),
        flac_block(6, struct.pack(">II", 3, 9) + b"image/png" + struct.pack(">I", 64) + b"desc", True),
    )
    picture_image_truncated = flac_file(
        flac_block(0, streaminfo),
        flac_block(6, flac_picture_block(b"image/png", b"cover", PNG_1X1[:4], len(PNG_1X1) + 64), True),
    )
    invalid_key_then_title = flac_file(
        flac_block(0, streaminfo),
        flac_block(4, vorbis_block([b"BAD\xffKEY=value", b"TITLE=after invalid key"]), True),
    )
    invalid_value_then_artist = flac_file(
        flac_block(0, streaminfo),
        flac_block(4, vorbis_block([b"TITLE=bad\xff", b"ARTIST=after invalid value"]), True),
    )
    invalid_lyrics_then_title = flac_file(
        flac_block(0, streaminfo),
        flac_block(4, vorbis_block([b"LYRICS=bad\xff", b"TITLE=after invalid lyrics"]), True),
    )

    write_seed(out_dir, "flac", "flac_valid_chain.flac", valid)
    write_seed(out_dir, "flac", "flac_truncated_block.flac", truncated)
    write_seed(out_dir, "flac", "flac_oversized_block.flac", oversized)
    write_seed(out_dir, "flac", "flac_picture_valid.flac", picture_valid)
    write_seed(out_dir, "flac", "flac_picture_mime_truncated.flac", picture_mime_truncated)
    write_seed(out_dir, "flac", "flac_picture_desc_truncated.flac", picture_desc_truncated)
    write_seed(out_dir, "flac", "flac_picture_image_truncated.flac", picture_image_truncated)
    write_seed(out_dir, "flac", "flac_vorbis_invalid_key_then_title.flac", invalid_key_then_title)
    write_seed(out_dir, "flac", "flac_vorbis_invalid_value_then_artist.flac", invalid_value_then_artist)
    write_seed(out_dir, "flac", "flac_vorbis_invalid_lyrics_then_title.flac", invalid_lyrics_then_title)


def generate_ogg(out_dir: Path) -> None:
    serial = 0x12345678
    ident = b"\x01vorbis" + bytes(23)
    comment = b"\x03vorbis" + vorbis_block([b"TITLE=ogg"]) + b"\x01"
    valid = ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, [len(comment)], comment)
    truncated = b"OggS\0\0" + bytes(10)
    continuation = ogg_page(serial, 0, 1, [255], b"A" * 255) + ogg_page(serial, 2, 1, [255], b"B" * 255)
    truncated_payload = ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, [len(comment)], comment[:-1])
    oversized_payload = ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, [255], b"short")
    invalid_key_comment = b"\x03vorbis" + vorbis_block([b"BAD\xffKEY=value", b"TITLE=after invalid key"]) + b"\x01"
    invalid_value_comment = b"\x03vorbis" + vorbis_block([b"TITLE=bad\xff", b"ARTIST=after invalid value"]) + b"\x01"
    invalid_lyrics_comment = b"\x03vorbis" + vorbis_block([b"LYRICS=bad\xff", b"TITLE=after invalid lyrics"]) + b"\x01"
    invalid_key = ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, [len(invalid_key_comment)], invalid_key_comment)
    invalid_value = ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, [len(invalid_value_comment)], invalid_value_comment)
    invalid_lyrics = ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, [len(invalid_lyrics_comment)], invalid_lyrics_comment)

    write_seed(out_dir, "ogg", "ogg_valid_vorbis_pages.ogg", valid)
    write_seed(out_dir, "ogg", "ogg_truncated_page.ogg", truncated)
    write_seed(out_dir, "ogg", "ogg_bad_continuation.ogg", continuation)
    write_seed(out_dir, "ogg", "ogg_truncated_payload_boundary.ogg", truncated_payload)
    write_seed(out_dir, "ogg", "ogg_oversized_payload_boundary.ogg", oversized_payload)
    write_seed(out_dir, "ogg", "ogg_vorbis_invalid_key_then_title.ogg", invalid_key)
    write_seed(out_dir, "ogg", "ogg_vorbis_invalid_value_then_artist.ogg", invalid_value)
    write_seed(out_dir, "ogg", "ogg_vorbis_invalid_lyrics_then_title.ogg", invalid_lyrics)


def generate_mp4(out_dir: Path) -> None:
    title = atom(b"\xa9nam", data_atom(1, b"mp4 title"))
    valid = mp4_metadata_file(title)
    truncated = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + struct.pack(">I4s", 64, b"moov") + b"short"
    deep = atom(b"free", b"deep")
    for _ in range(96):
        deep = atom(b"moov", deep)
    deep = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + deep
    deep_nesting = atom(b"free", b"matrix")
    for index in range(160):
        deep_nesting = atom(b"moov" if index % 2 == 0 else b"udta", deep_nesting)
    deep_nesting = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + deep_nesting
    multi_data = mp4_metadata_file(atom(b"\xa9nam", data_atom(1, b"") + data_atom(1, b"mp4 second title")))
    meta_too_small = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + atom(b"moov", atom(b"udta", atom(b"meta", b"\0\0\0")))
    meta_version_1 = mp4_metadata_file(title, b"\1\0\0\0")
    extended = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + extended_atom(
        b"moov", atom(b"udta", atom(b"meta", b"\0\0\0\0" + atom(b"ilst", title)))
    )
    nested_item = atom(b"\xa9nam", atom(b"free", data_atom(1, b"ignored nested data")) + data_atom(1, b"mp4 nested sibling"))
    deep_metadata = mp4_metadata_file(nested_item)
    covr_invalid_then_valid = mp4_metadata_file(atom(b"covr", data_atom(13, b"bad-image") + data_atom(14, PNG_1X1)))

    write_seed(out_dir, "mp4", "mp4_valid_tree.m4a", valid)
    write_seed(out_dir, "mp4", "mp4_truncated_atom.m4a", truncated)
    write_seed(out_dir, "mp4", "mp4_deep_atoms.m4a", deep)
    write_seed(out_dir, "mp4", "mp4_deep_nesting_matrix.m4a", deep_nesting)
    write_seed(out_dir, "mp4", "mp4_multi_data_first_empty.m4a", multi_data)
    write_seed(out_dir, "mp4", "mp4_meta_payload_too_small.m4a", meta_too_small)
    write_seed(out_dir, "mp4", "mp4_meta_version_1.m4a", meta_version_1)
    write_seed(out_dir, "mp4", "mp4_extended_atom.m4a", extended)
    write_seed(out_dir, "mp4", "mp4_deep_metadata_item.m4a", deep_metadata)
    write_seed(out_dir, "mp4", "mp4_covr_invalid_then_valid.m4a", covr_invalid_then_valid)


def generate_image(out_dir: Path) -> None:
    apic = b"\x03image/png\0\x03cover\0" + PNG_1X1
    valid = id3_tag(4, id3v24_frame(b"APIC", apic))
    truncated = id3_tag(4, id3v24_frame(b"APIC", b"\x03image/png\0\x03cover\0" + PNG_1X1[:12]))
    oversized = id3_tag(4, id3v24_frame(b"APIC", b"\x03image/png\0\x03cover\0" + b"x" * (1024 * 1024 + 1)))
    write_seed(out_dir, "image", "image_apic_png.mp3", valid)
    write_seed(out_dir, "image", "image_apic_truncated.mp3", truncated)
    write_seed(out_dir, "image", "image_apic_large_payload.mp3", oversized)


def generate_encoding(out_dir: Path) -> None:
    utf8 = id3_tag(4, id3v24_frame(b"TIT2", b"\x03utf8 title"))
    utf16 = id3_tag(4, id3v24_frame(b"TIT2", b"\x01\xff\xfeU\0T\0F\0"))
    invalid = id3_tag(4, id3v24_frame(b"TIT2", b"\x03\xff\xfe\xff"))
    large_latin1 = id3_tag(3, id3v23_frame(b"TIT2", b"\x00" + b"A" * (MAX_DECODED_TEXT_BYTES // 4 + 1)))
    utf16_odd = id3_tag(3, id3v23_frame(b"TIT2", b"\x01\xff\xfeA\0B"))
    mp4_utf16_title = atom(b"\xa9nam", data_atom(2, b"\0A" * (MAX_TEXT_FIELD_BYTES // 2 + 1)))
    mp4_utf16_over_limit = mp4_metadata_file(mp4_utf16_title)
    write_seed(out_dir, "encoding", "encoding_utf8.mp3", utf8)
    write_seed(out_dir, "encoding", "encoding_utf16.mp3", utf16)
    write_seed(out_dir, "encoding", "encoding_invalid_utf8.mp3", invalid)
    write_seed(out_dir, "encoding", "id3v23_large_latin1_text.mp3", large_latin1)
    write_seed(out_dir, "encoding", "id3v23_utf16_odd_length.mp3", utf16_odd)
    write_seed(out_dir, "encoding", "mp4_utf16_title_over_limit.m4a", mp4_utf16_over_limit)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate TagReader fuzz corpus seeds")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    args = parser.parse_args()

    if args.out_dir.exists():
        shutil.rmtree(args.out_dir)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    generate_id3(args.out_dir)
    generate_flac(args.out_dir)
    generate_ogg(args.out_dir)
    generate_mp4(args.out_dir)
    generate_image(args.out_dir)
    generate_encoding(args.out_dir)

    for category_dir in sorted(p for p in args.out_dir.iterdir() if p.is_dir()):
        count = sum(1 for p in category_dir.iterdir() if p.is_file())
        print(f"{category_dir.name}: {count}")
    print(f"generated corpus under {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
