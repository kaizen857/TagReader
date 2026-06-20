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
MAX_LYRICS_BYTES = 8 * 1024 * 1024
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


def utf16be_bom_text(text: str) -> bytes:
    return b"\xfe\xff" + text.encode("utf-16-be")


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


def ogg_segments(size: int) -> list[int]:
    segments = [255] * (size // 255)
    segments.append(size % 255)
    return segments


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


def flac_picture_comment(image: bytes = PNG_1X1, mime: bytes = b"image/png", description: bytes = b"cover") -> bytes:
    return b"METADATA_BLOCK_PICTURE=" + base64.b64encode(flac_picture_block(mime, description, image))


def lrc_line(timestamp_count: int, text: bytes = b"line") -> bytes:
    return b"".join(f"[00:{index % 60:02d}.00]".encode() for index in range(timestamp_count)) + text + b"\n"


def riff_chunk(chunk_id: bytes, payload: bytes, declared_size: int | None = None) -> bytes:
    size = len(payload) if declared_size is None else declared_size
    chunk = chunk_id + struct.pack("<I", size) + payload
    if declared_size is None and len(payload) % 2:
        chunk += b"\0"
    return chunk


def riff_file(chunks: list[bytes], form_type: bytes = b"WAVE") -> bytes:
    payload = form_type + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(payload)) + payload


def riff_info_field(field_id: bytes, value: bytes) -> bytes:
    return riff_chunk(field_id, value + b"\0")


def riff_info_list(fields: list[bytes], declared_size: int | None = None) -> bytes:
    return riff_chunk(b"LIST", b"INFO" + b"".join(fields), declared_size)


def aiff_chunk(chunk_id: bytes, payload: bytes, declared_size: int | None = None) -> bytes:
    size = len(payload) if declared_size is None else declared_size
    chunk = chunk_id + struct.pack(">I", size) + payload
    if declared_size is None and len(payload) % 2:
        chunk += b"\0"
    return chunk


def aiff_text_chunk(chunk_id: bytes, value: bytes) -> bytes:
    return aiff_chunk(chunk_id, value)


def aiff_comm_chunk() -> bytes:
    return aiff_chunk(b"COMM", b"\0\1" + struct.pack(">I", 1) + b"\0\x10" + bytes.fromhex("400eac44000000000000"))


def aiff_ssnd_chunk() -> bytes:
    return aiff_chunk(b"SSND", struct.pack(">II", 0, 0) + b"\0\0")


def aiff_file(form_type: bytes, chunks: list[bytes], declared_size: int | None = None) -> bytes:
    payload = form_type + b"".join(chunks)
    size = len(payload) if declared_size is None else declared_size
    return b"FORM" + struct.pack(">I", size) + payload


def dsf_file(metadata: bytes, metadata_pointer: int, declared_file_size: int | None = None) -> bytes:
    header_size = 28
    file_size = declared_file_size if declared_file_size is not None else metadata_pointer + len(metadata)
    data = bytearray(b"DSD ")
    data += struct.pack("<Q", header_size)
    data += struct.pack("<Q", file_size)
    data += struct.pack("<Q", metadata_pointer)
    if metadata_pointer > len(data):
        data.extend(b"\0" * (metadata_pointer - len(data)))
    data += metadata
    return bytes(data)


def dff_chunk(chunk_id: bytes, payload: bytes, declared_size: int | None = None) -> bytes:
    size = len(payload) if declared_size is None else declared_size
    chunk = chunk_id + struct.pack(">Q", size) + payload
    if declared_size is None and len(payload) % 2:
        chunk += b"\0"
    return chunk


def dff_file(chunks: list[bytes], form_type: bytes = b"DSD ") -> bytes:
    return dff_chunk(b"FRM8", form_type + b"".join(chunks))


ASF_HEADER_GUID = bytes.fromhex("3026b2758e66cf11a6d900aa0062ce6c")
ASF_CONTENT_DESCRIPTION_GUID = bytes.fromhex("3326b2758e66cf11a6d900aa0062ce6c")
ASF_EXTENDED_CONTENT_DESCRIPTION_GUID = bytes.fromhex("40a4d0d207e3d21197f000a0c95ea850")
ASF_METADATA_LIBRARY_GUID = bytes.fromhex("941c23449894d149a1411d134e457054")


def utf16le_text(text: str, terminated: bool = True) -> bytes:
    encoded = text.encode("utf-16-le")
    return encoded + (b"\0\0" if terminated else b"")


def asf_object(guid: bytes, payload: bytes, declared_size: int | None = None) -> bytes:
    size = len(payload) + 24 if declared_size is None else declared_size
    return guid + struct.pack("<Q", size) + payload


def asf_header_object(children: list[bytes], declared_size: int | None = None) -> bytes:
    payload = struct.pack("<I", len(children)) + b"\x01\x02" + b"".join(children)
    return asf_object(ASF_HEADER_GUID, payload, declared_size)


def asf_content_description(title: str, author: str, copyright_text: str, description: str, rating: str) -> bytes:
    fields = [utf16le_text(field) for field in (title, author, copyright_text, description, rating)]
    payload = b"".join(struct.pack("<H", len(field)) for field in fields) + b"".join(fields)
    return asf_object(ASF_CONTENT_DESCRIPTION_GUID, payload)


def asf_extended_descriptor(name: str, value_type: int, value: bytes, declared_value_len: int | None = None) -> bytes:
    name_bytes = utf16le_text(name)
    value_len = len(value) if declared_value_len is None else declared_value_len
    return struct.pack("<H", len(name_bytes)) + name_bytes + struct.pack("<HH", value_type, value_len) + value


def asf_extended_content_description(descriptors: list[bytes]) -> bytes:
    return asf_object(ASF_EXTENDED_CONTENT_DESCRIPTION_GUID, struct.pack("<H", len(descriptors)) + b"".join(descriptors))


def asf_metadata_library_descriptor(name: str, value_type: int, value: bytes, declared_value_len: int | None = None) -> bytes:
    name_bytes = utf16le_text(name)
    value_len = len(value) if declared_value_len is None else declared_value_len
    return struct.pack("<HHHHI", 0, 0, len(name_bytes), value_type, value_len) + name_bytes + value


def asf_metadata_library(descriptors: list[bytes]) -> bytes:
    return asf_object(ASF_METADATA_LIBRARY_GUID, struct.pack("<H", len(descriptors)) + b"".join(descriptors))


def asf_picture_value(image: bytes, declared_image_len: int | None = None) -> bytes:
    image_len = len(image) if declared_image_len is None else declared_image_len
    return b"\x03" + struct.pack("<I", image_len) + utf16le_text("image/png") + utf16le_text("front cover") + image


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


def matroska_unknown_size_element(element_id: int, payload: bytes) -> bytes:
    return matroska_id(element_id) + b"\xff" + payload


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


def write_seed(out_dir: Path, category: str, name: str, data: bytes) -> None:
    category_dir = out_dir / category
    category_dir.mkdir(parents=True, exist_ok=True)
    (category_dir / name).write_bytes(data)


def generate_id3(out_dir: Path) -> None:
    v22 = id3_tag(2, id3v22_frame(b"TT2", b"\x03v22 title"))
    v23 = id3_tag(3, id3v23_frame(b"TIT2", b"\x03v23 title"))
    v24 = id3_tag(4, id3v24_frame(b"TIT2", b"\x03v24 title"))
    v22_flagged_lyrics = id3_tag(2, id3v22_frame(b"ULT", b"\x03eng\0ID3v22 flagged lyric line"), 0x80)
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
    lrc_bracket_plain = id3_tag(
        3,
        id3v23_frame(b"USLT", b"\x03eng\0[ar:Unit Test Artist]\n[Verse]\n[hello]\n[Chorus] sing"),
    )
    lrc_timed_multi = id3_tag(
        3,
        id3v23_frame(b"USLT", b"\x03eng\0[00:01.00]first timed line\n[00:02.00]second timed line"),
    )
    plain_over_limit = id3_tag(3, id3v23_frame(b"USLT", b"\x03eng\0" + b"P" * (MAX_PLAIN_LYRICS_BYTES + 1)))
    slt_many_lines = id3_tag(2, id3v22_frame(b"SLT", slt_many_lines_payload))
    sylt_many_lines = id3_tag(3, id3v23_frame(b"SYLT", sylt_many_lines_payload))
    public_api_multi_field = id3_tag(
        3,
        id3v23_frame(b"TIT2", b"\x03public title")
        + id3v23_frame(b"TPE1", b"\x03public artist")
        + id3v23_frame(b"TALB", b"\x03public album")
        + id3v23_frame(b"TRCK", b"\x0312/34")
        + id3v23_frame(b"TPOS", b"\x032/5")
        + id3v23_frame(b"USLT", b"\x03eng\0[00:01.25]public lyric\nplain fallback"),
    )
    public_api_txxx_lrc = id3_tag(
        4,
        id3v24_frame(b"TXXX", b"\x03LYRICS\0[00:00.00]first\n[00:00.50][00:01.00]repeat"),
    )

    write_seed(out_dir, "id3", "id3v22_minimal.mp3", v22)
    write_seed(out_dir, "id3", "id3v22_lyrics_flagged.mp3", v22_flagged_lyrics)
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
    write_seed(out_dir, "id3", "id3v23_lrc_bracket_plain.mp3", lrc_bracket_plain)
    write_seed(out_dir, "id3", "id3v23_lrc_timed_multi.mp3", lrc_timed_multi)
    write_seed(out_dir, "id3", "id3v23_plain_lyrics_over_limit.mp3", plain_over_limit)
    write_seed(out_dir, "id3", "id3v22_slt_line_cap.mp3", slt_many_lines)
    write_seed(out_dir, "id3", "id3v23_sylt_line_cap.mp3", sylt_many_lines)
    write_seed(out_dir, "id3", "id3v23_public_api_multi_field.mp3", public_api_multi_field)
    write_seed(out_dir, "id3", "id3v24_public_api_txxx_lrc.mp3", public_api_txxx_lrc)


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
    picture_comment = b"\x03vorbis" + vorbis_block([b"TITLE=ogg picture", flac_picture_comment()]) + b"\x01"
    picture_malformed_comment = b"\x03vorbis" + vorbis_block([b"TITLE=ogg malformed picture", b"METADATA_BLOCK_PICTURE=not@base64"]) + b"\x01"
    public_api_comment = b"\x03vorbis" + vorbis_block(
        [
            b"TITLE=ogg public title",
            b"ARTIST=ogg public artist",
            b"ALBUM=ogg public album",
            b"TRACKNUMBER=7/12",
            b"DISCNUMBER=1/2",
            b"LYRICS=[00:02.00]ogg timed lyric\nplain ogg lyric",
            b"UNSYNCEDLYRICS=unsynced fallback",
        ]
    ) + b"\x01"
    public_api = ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, [len(public_api_comment)], public_api_comment)
    multistream_serial = 0x87654321
    opus_head = b"OpusHead" + b"\x01\x01" + b"\x00" * 16
    opus_tags = b"OpusTags" + vorbis_block([b"TITLE=opus title", b"ARTIST=opus artist", flac_picture_comment()], b"opus-vendor")
    opus_picture = ogg_page(0x0BADF00D, 0, 0x02, [len(opus_head)], opus_head) + ogg_page(
        0x0BADF00D, 1, 0, ogg_segments(len(opus_tags)), opus_tags
    )
    opus_truncated_tags = ogg_page(0x0BADF00D, 0, 0x02, [len(opus_head)], opus_head) + ogg_page(
        0x0BADF00D, 1, 0, [8], b"OpusTags"
    )
    multistream_comment = b"\x03vorbis" + vorbis_block(
        [
            b"TITLE=ogg multistream title",
            b"ARTIST=ogg multistream artist",
            b"ALBUM=ogg multistream album",
            b"LYRICS=ogg multistream lyric",
        ]
    ) + b"\x01"
    multistream = (
        ogg_page(0x0BADF00D, 0, 0x02, [len(opus_head)], opus_head)
        + ogg_page(multistream_serial, 0, 0x02, [len(ident)], ident)
        + ogg_page(multistream_serial, 1, 0, ogg_segments(len(multistream_comment)), multistream_comment)
    )
    resource_limit = ogg_page(serial, 0, 0x02, [len(ident)], ident) + b"".join(
        ogg_page(serial, sequence, 0x01 if sequence > 1 else 0, [255], b"R" * 255) for sequence in range(1, 512)
    )

    write_seed(out_dir, "ogg", "ogg_valid_vorbis_pages.ogg", valid)
    write_seed(out_dir, "ogg", "ogg_truncated_page.ogg", truncated)
    write_seed(out_dir, "ogg", "ogg_bad_continuation.ogg", continuation)
    write_seed(out_dir, "ogg", "ogg_truncated_payload_boundary.ogg", truncated_payload)
    write_seed(out_dir, "ogg", "ogg_oversized_payload_boundary.ogg", oversized_payload)
    write_seed(out_dir, "ogg", "ogg_vorbis_invalid_key_then_title.ogg", invalid_key)
    write_seed(out_dir, "ogg", "ogg_vorbis_invalid_value_then_artist.ogg", invalid_value)
    write_seed(out_dir, "ogg", "ogg_vorbis_invalid_lyrics_then_title.ogg", invalid_lyrics)
    write_seed(out_dir, "ogg", "ogg_vorbis_metadata_block_picture.ogg", ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, ogg_segments(len(picture_comment)), picture_comment))
    write_seed(out_dir, "ogg", "ogg_vorbis_malformed_picture_comment.ogg", ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, ogg_segments(len(picture_malformed_comment)), picture_malformed_comment))
    write_seed(out_dir, "ogg", "ogg_opus_opustags_picture.opus", opus_picture)
    write_seed(out_dir, "ogg", "ogg_opus_truncated_opustags.opus", opus_truncated_tags)
    write_seed(out_dir, "ogg", "ogg_public_api_comments_and_lrc.ogg", public_api)
    write_seed(out_dir, "ogg", "ogg_vorbis_multistream_target_comments.ogg", multistream)
    write_seed(out_dir, "ogg", "ogg_comment_resource_limit.ogg", resource_limit)


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
    public_api_item = (
        atom(b"\xa9nam", data_atom(1, b"mp4 public title"))
        + atom(b"\xa9ART", data_atom(1, b"mp4 public artist"))
        + atom(b"\xa9alb", data_atom(1, b"mp4 public album"))
        + atom(b"trkn", data_atom(0, b"\0\0\0\7\0\f\0\0"))
        + atom(b"disk", data_atom(0, b"\0\0\0\1\0\2\0\0"))
        + atom(b"\xa9lyr", data_atom(1, b"[00:03.00]mp4 timed lyric\nmp4 plain lyric"))
    )
    freeform_lyrics = atom(
        b"----",
        atom(b"mean", b"\0\0\0\0com.apple.iTunes")
        + atom(b"name", b"\0\0\0\0Lyrics")
        + data_atom(1, b"[00:04.50]freeform lyric"),
    )
    public_api = mp4_metadata_file(public_api_item + freeform_lyrics)
    zero_size = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + atom(b"moov", atom(b"udta", atom(b"meta", b"\0\0\0\0" + atom(b"ilst", struct.pack(">I4s", 0, b"free") + title))))
    size0_tail = mp4_metadata_file(
        atom(b"\xa9nam", data_atom(1, b"Size0 Tail OK"))
        + atom(b"\xa9ART", data_atom(1, b"Size0 Artist"))
        + struct.pack(">I4s", 0, b"free")
    )
    size0_hidden_sibling = mp4_metadata_file(
        struct.pack(">I4s", 0, b"free")
        + b"bad!"
        + atom(b"\xa9nam", data_atom(1, b"After Size0"))
        + atom(b"\xa9ART", data_atom(1, b"Recovered Artist"))
    )
    lyrics_utf16 = mp4_metadata_file(atom(b"\xa9lyr", data_atom(2, utf16be_bom_text("MP4 UTF16 lyric line"))))
    lyrics_utf8 = mp4_metadata_file(atom(b"\xa9lyr", data_atom(1, b"MP4 UTF8 lyric line")))
    lyrics_oversized = mp4_metadata_file(atom(b"\xa9lyr", data_atom(1, b"O" * (MAX_LYRICS_BYTES + 1))))
    lyrics_utf16_odd = mp4_metadata_file(atom(b"\xa9lyr", data_atom(2, b"\xfe\xff\0M\0")))
    freeform_lyrics_utf16 = mp4_metadata_file(
        atom(
            b"----",
            atom(b"mean", b"\0\0\0\0com.apple.iTunes")
            + atom(b"name", b"\0\0\0\0Lyrics")
            + data_atom(2, utf16be_bom_text("MP4 UTF16 lyric line")),
        )
    )

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
    write_seed(out_dir, "mp4", "mp4_public_api_metadata_and_lyrics.m4a", public_api)
    write_seed(out_dir, "mp4", "mp4_zero_size_atom_before_title.m4a", zero_size)
    write_seed(out_dir, "mp4", "mp4_size0_tail_ok.m4a", size0_tail)
    write_seed(out_dir, "mp4", "mp4_size0_hides_metadata.m4a", size0_hidden_sibling)
    write_seed(out_dir, "mp4", "mp4_lyrics_utf16_bom.m4a", lyrics_utf16)
    write_seed(out_dir, "mp4", "mp4_lyrics_utf8.m4a", lyrics_utf8)
    write_seed(out_dir, "mp4", "mp4_lyrics_oversized.m4a", lyrics_oversized)
    write_seed(out_dir, "mp4", "mp4_lyrics_utf16_odd.m4a", lyrics_utf16_odd)
    write_seed(out_dir, "mp4", "mp4_freeform_lyrics_utf16_bom.m4a", freeform_lyrics_utf16)


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


def generate_riff(out_dir: Path) -> None:
    info = riff_info_list(
        [
            riff_info_field(b"INAM", b"wav title"),
            riff_info_field(b"IART", b"wav artist"),
            riff_info_field(b"IPRD", b"wav album"),
        ]
    )
    embedded_id3 = riff_chunk(b"id3 ", id3_tag(3, id3v23_frame(b"TIT2", b"\x03embedded wav title")))
    malformed_list = riff_info_list([riff_info_field(b"INAM", b"truncated")], declared_size=64)
    oversized_list = riff_info_list([riff_info_field(b"INAM", b"oversized")], declared_size=17 * 1024 * 1024)

    write_seed(out_dir, "riff", "wav_info_list.wav", riff_file([info]))
    write_seed(out_dir, "riff", "wav_embedded_id3.wav", riff_file([embedded_id3, info]))
    write_seed(out_dir, "riff", "wav_malformed_list_size.wav", riff_file([malformed_list]))
    write_seed(out_dir, "riff", "wav_oversized_list_declared.wav", riff_file([oversized_list]))


def generate_aiff(out_dir: Path) -> None:
    native = [
        aiff_comm_chunk(),
        aiff_ssnd_chunk(),
        aiff_text_chunk(b"NAME", b"aiff title"),
        aiff_text_chunk(b"AUTH", b"aiff artist"),
        aiff_text_chunk(b"ANNO", b"aiff note"),
    ]
    embedded_id3 = aiff_chunk(b"ID3 ", id3_tag(3, id3v23_frame(b"TIT2", b"\x03embedded aiff title")))
    truncated_comt = aiff_chunk(b"COMT", b"\0\1\0\0", declared_size=32)
    oversized_native = aiff_chunk(b"NAME", b"oversized", declared_size=17 * 1024 * 1024)

    write_seed(out_dir, "aiff", "aiff_native_chunks.aiff", aiff_file(b"AIFF", native))
    write_seed(out_dir, "aiff", "aifc_embedded_id3.aifc", aiff_file(b"AIFC", [aiff_comm_chunk(), aiff_ssnd_chunk(), embedded_id3]))
    write_seed(out_dir, "aiff", "aiff_truncated_comt.aiff", aiff_file(b"AIFF", [aiff_comm_chunk(), aiff_ssnd_chunk(), truncated_comt]))
    write_seed(out_dir, "aiff", "aiff_oversized_native_chunk.aiff", aiff_file(b"AIFF", [aiff_comm_chunk(), aiff_ssnd_chunk(), oversized_native]))
    write_seed(out_dir, "aiff", "aiff_bad_form_size.aiff", aiff_file(b"AIFF", [aiff_comm_chunk()], declared_size=0xFFFFFFFF))


def generate_dsd(out_dir: Path) -> None:
    id3 = id3_tag(3, id3v23_frame(b"TIT2", b"\x03dsd title"))
    write_seed(out_dir, "dsd", "dsf_id3_pointer.dsf", dsf_file(id3, 64))
    write_seed(out_dir, "dsd", "dsf_zero_metadata_pointer.dsf", dsf_file(b"", 0, 28))
    write_seed(out_dir, "dsd", "dsf_pointer_out_of_bounds.dsf", dsf_file(id3, 64, 32))
    write_seed(out_dir, "dsd", "dff_id3_chunk.dff", dff_file([dff_chunk(b"ID3 ", id3)]))
    write_seed(out_dir, "dsd", "dff_di3v_chunk.dff", dff_file([dff_chunk(b"DI3v", id3)]))
    write_seed(out_dir, "dsd", "dff_empty_standard_tags.dff", dff_file([dff_chunk(b"PROP", b"SND " + dff_chunk(b"FS  ", struct.pack(">I", 2822400)))]))


def generate_asf(out_dir: Path) -> None:
    metadata = asf_header_object(
        [
            asf_content_description("asf title", "asf author", "", "asf description", ""),
            asf_extended_content_description(
                [
                    asf_extended_descriptor("WM/AlbumTitle", 0, utf16le_text("asf album")),
                    asf_extended_descriptor("WM/Lyrics", 0, utf16le_text("asf lyric")),
                    asf_extended_descriptor("WM/Picture", 1, asf_picture_value(PNG_1X1)),
                ]
            ),
        ]
    )
    malformed_utf16 = asf_header_object(
        [asf_extended_content_description([asf_extended_descriptor("Title", 0, utf16le_text("bad", False) + b"\0")])]
    )
    oversized_image = asf_header_object(
        [asf_metadata_library([asf_metadata_library_descriptor("WM/Picture", 1, asf_picture_value(b"", 64 * 1024 * 1024 + 1))])]
    )
    oversized_object = asf_header_object([], declared_size=64 * 1024 * 1024 + 25)

    write_seed(out_dir, "asf", "asf_metadata_picture.wma", metadata)
    write_seed(out_dir, "asf", "asf_malformed_utf16_descriptor.wma", malformed_utf16)
    write_seed(out_dir, "asf", "asf_oversized_picture_declared.wma", oversized_image)
    write_seed(out_dir, "asf", "asf_oversized_header_object.wma", oversized_object)
    write_seed(out_dir, "asf", "asf_bad_magic.wma", b"not an ASF object")


def generate_matroska(out_dir: Path) -> None:
    tags = matroska_element(
        0x1254C367,
        matroska_element(
            0x7373,
            matroska_simple_tag("TITLE", "matroska title")
            + matroska_simple_tag("ARTIST", "matroska artist")
            + matroska_simple_tag("DATE_RELEASED", "2026-06-19"),
        ),
    )
    attachments = matroska_element(
        0x1941A469,
        matroska_attached_file("cover.txt", "text/plain", b"not a cover")
        + matroska_attached_file("cover.png", "image/png", PNG_1X1),
    )
    unknown_size = matroska_file(matroska_unknown_size_element(0xEC, b"unknown sized local padding"))
    malformed_size = matroska_element(0x1A45DFA3, matroska_text_element(0x4282, "matroska")) + matroska_id(0x18538067) + b"\x84bad"

    write_seed(out_dir, "matroska", "matroska_simple_tags.mka", matroska_file(tags))
    write_seed(out_dir, "matroska", "matroska_attachment_cover.webm", matroska_file(tags + attachments))
    write_seed(out_dir, "matroska", "matroska_unknown_size_element.mka", unknown_size)
    write_seed(out_dir, "matroska", "matroska_malformed_segment_size.mka", malformed_size)


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
    generate_riff(args.out_dir)
    generate_aiff(args.out_dir)
    generate_dsd(args.out_dir)
    generate_asf(args.out_dir)
    generate_matroska(args.out_dir)

    for category_dir in sorted(p for p in args.out_dir.iterdir() if p.is_dir()):
        count = sum(1 for p in category_dir.iterdir() if p.is_file())
        print(f"{category_dir.name}: {count}")
    print(f"generated corpus under {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
