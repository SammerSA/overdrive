# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Hash-pinned, unmodified 7-Zip extraction sources for the separate archive worker."""
import tarfile
from common import DEPS, LOCK, fetch


def sevenzip_source():
    pinned = LOCK["sevenZipSource"]
    archive = fetch(pinned["url"], DEPS / f"7z-{pinned['version']}-src.tar.xz", pinned["sha256"])
    root = DEPS / f"7zip-{pinned['version']}"
    marker = root / ".extracted"
    if not marker.exists() or marker.read_text() != pinned["sha256"]:
        root.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as package:
            package.extractall(root, filter="data")
        marker.write_text(pinned["sha256"])
    return root


# Extraction-only codecs. No encoders, CLI, crypto or 7-Zip RAR decoder are linked.
SOURCES = {
    "C": "7zCrc 7zCrcOpt 7zStream Alloc Bcj2 Bra Bra86 BraIA64 CpuArch Delta Lzma2Dec Lzma2DecMt LzmaDec MtDec Ppmd7 Ppmd7Dec SwapBytes Threads ZstdDec Xxh64",
    "CPP/Common": "CRC IntToString MyWindows MyString MyVector StringConvert StringToInt UTFConvert",
    "CPP/Windows": "PropVariant System",
    "CPP/7zip/Common": "CreateCoder CWrappers InBuffer FilterCoder LimitedStreams MethodId MethodProps OutBuffer ProgressUtils PropId StreamBinder StreamObjects StreamUtils VirtThread",
    "CPP/7zip/Archive/Common": "CoderMixer2 HandlerOut ItemNameUtils OutStreamWithCRC ParseProperties",
    "CPP/7zip/Archive/7z": "7zCompressionMode 7zDecode 7zExtract 7zHandler 7zHeader 7zIn 7zProperties",
    "CPP/7zip/Compress": "Bcj2Coder Bcj2Register BcjCoder BcjRegister BitlDecoder BranchMisc BranchRegister ByteSwap BZip2Crc BZip2Decoder BZip2Register CopyCoder CopyRegister DeflateDecoder DeflateRegister DeltaFilter Lzma2Decoder Lzma2Register LzmaDecoder LzmaRegister LzOutWindow PpmdDecoder PpmdRegister ZstdDecoder",
}


def source_files(root):
    return [root / folder / (name + (".c" if folder == "C" else ".cpp"))
            for folder, names in SOURCES.items() for name in names.split()]
