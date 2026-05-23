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


def data_atom(data_type: int, payload: bytes) -> bytes:
    return atom(b"data", struct.pack(">II", data_type, 0) + payload)


def id3v22_frame(frame_id: bytes, payload: bytes) -> bytes:
    return frame_id + len(payload).to_bytes(3, "big") + payload


def id3v23_frame(frame_id: bytes, payload: bytes) -> bytes:
    return frame_id + len(payload).to_bytes(4, "big") + b"\0\0" + payload


def id3v24_frame(frame_id: bytes, payload: bytes) -> bytes:
    return frame_id + syncsafe32(len(payload)) + b"\0\0" + payload


def id3_tag(version: int, frames: bytes) -> bytes:
    return b"ID3" + bytes([version, 0, 0]) + syncsafe32(len(frames)) + frames


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
    write_seed(out_dir, "id3", "id3v22_minimal.mp3", v22)
    write_seed(out_dir, "id3", "id3v23_minimal.mp3", v23)
    write_seed(out_dir, "id3", "id3v24_minimal.mp3", v24)
    write_seed(out_dir, "id3", "id3v24_truncated.mp3", truncated)
    write_seed(out_dir, "id3", "id3v24_oversized.mp3", oversized)


def generate_flac(out_dir: Path) -> None:
    streaminfo = bytes(34)
    vorbis = struct.pack("<I", 6) + b"vendor" + struct.pack("<I", 1) + struct.pack("<I", 11) + b"TITLE=flac"
    valid = b"fLaC" + bytes([0x00]) + len(streaminfo).to_bytes(3, "big") + streaminfo + bytes([0x84]) + len(vorbis).to_bytes(3, "big") + vorbis
    truncated = b"fLaC" + bytes([0x04]) + (32).to_bytes(3, "big") + b"short"
    oversized = b"fLaC" + bytes([0x04]) + (0xFFFFFF).to_bytes(3, "big") + b"x"
    write_seed(out_dir, "flac", "flac_valid_chain.flac", valid)
    write_seed(out_dir, "flac", "flac_truncated_block.flac", truncated)
    write_seed(out_dir, "flac", "flac_oversized_block.flac", oversized)


def generate_ogg(out_dir: Path) -> None:
    serial = 0x12345678
    ident = b"\x01vorbis" + bytes(23)
    comment = b"\x03vorbis" + struct.pack("<I", 6) + b"vendor" + struct.pack("<I", 1) + struct.pack("<I", 9) + b"TITLE=ogg" + b"\x01"
    valid = ogg_page(serial, 0, 0, [len(ident)], ident) + ogg_page(serial, 1, 0, [len(comment)], comment)
    truncated = b"OggS\0\0" + bytes(10)
    continuation = ogg_page(serial, 0, 1, [255], b"A" * 255) + ogg_page(serial, 2, 1, [255], b"B" * 255)
    write_seed(out_dir, "ogg", "ogg_valid_vorbis_pages.ogg", valid)
    write_seed(out_dir, "ogg", "ogg_truncated_page.ogg", truncated)
    write_seed(out_dir, "ogg", "ogg_bad_continuation.ogg", continuation)


def generate_mp4(out_dir: Path) -> None:
    title = atom(b"\xa9nam", data_atom(1, b"mp4 title"))
    valid = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + atom(b"moov", atom(b"udta", atom(b"meta", b"\0\0\0\0" + atom(b"ilst", title))))
    truncated = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + struct.pack(">I4s", 64, b"moov") + b"short"
    deep = atom(b"free", b"deep")
    for _ in range(96):
        deep = atom(b"moov", deep)
    deep = atom(b"ftyp", b"M4A \0\0\0\0M4A ") + deep
    write_seed(out_dir, "mp4", "mp4_valid_tree.m4a", valid)
    write_seed(out_dir, "mp4", "mp4_truncated_atom.m4a", truncated)
    write_seed(out_dir, "mp4", "mp4_deep_atoms.m4a", deep)


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
    write_seed(out_dir, "encoding", "encoding_utf8.mp3", utf8)
    write_seed(out_dir, "encoding", "encoding_utf16.mp3", utf16)
    write_seed(out_dir, "encoding", "encoding_invalid_utf8.mp3", invalid)


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
