"""Real SDK extraction: independent folders, LZMA2 reset blocks and safety failures."""
from pathlib import Path
import binascii
import hashlib
import lzma
import os
import re
import shutil
import struct
import subprocess
import tempfile
import threading
import time
from rar_worker import build_host
from test_archives import number7z


def archive7z(files, reset_blocks=False, dictionary=1 << 20, solid=1, delta=False, attributes=None, zstd=False):
    """Small independent 7z writer, deliberately not using the decoder under test.

    Each group of `solid` files has one folder. Reset-block mode joins independent LZMA2
    streams in that folder, dropping only their end markers as the format permits.
    """
    prop = next(p for p in range(40) if (2 | (p & 1)) << (p // 2 + 11) == dictionary)
    packed = []
    groups = [files[i:i + solid] for i in range(0, len(files), solid)]
    payloads = [b"".join(data for _, data in group) for group in groups]
    for data in payloads:
        if zstd:
            packed.append(subprocess.check_output(["zstd", "-q", "-c"], input=data))
            continue
        chunks = [data[i:i + 4 * dictionary] for i in range(0, len(data), 4 * dictionary)] if reset_blocks else [data]
        filters = ([{"id": lzma.FILTER_DELTA, "dist": 1}] if delta else []) + [{"id": lzma.FILTER_LZMA2, "dict_size": dictionary}]
        encoded = [lzma.compress(chunk, format=lzma.FORMAT_RAW, filters=filters) for chunk in chunks]
        packed.append(b"".join(chunk[:-1] for chunk in encoded) + b"\0")
    n = number7z(len(groups))
    header = (b"\x01\x04\x06\0" + n + b"\x09" + b"".join(number7z(len(p)) for p in packed) + b"\0"
              + b"\x07\x0b" + n + b"\0")
    folder = (b"\x02" if delta else b"\x01") + b"\x21\x21\x01" + bytes([prop])
    if zstd: folder = b"\x01\x04\x04\xf7\x11\x01"
    if delta: folder += b"\x21\x03\x01\0\x01\0"
    header += folder * len(groups)
    header += b"\x0c" + b"".join(number7z(len(data)) * (2 if delta else 1) for data in payloads)
    header += b"\x0a\x01" + b"".join(struct.pack("<I", binascii.crc32(data)) for data in payloads) + b"\0"
    if solid > 1:
        header += b"\x08\x0d" + b"".join(number7z(len(group)) for group in groups)
        header += b"\x09" + b"".join(number7z(len(data)) for group in groups for _, data in group[:-1])
        header += b"\x0a\x01" + b"".join(struct.pack("<I", binascii.crc32(data)) for group in groups if len(group) > 1 for _, data in group) + b"\0"
    header += b"\0"
    names = b"\0" + b"".join(name.encode("utf-16le") + b"\0\0" for name, _ in files)
    header += b"\x05" + number7z(len(files)) + b"\x11" + number7z(len(names)) + names
    if attributes:
        attrs = b"\x01\0" + b"".join(struct.pack("<I", a) for a in attributes)
        header += b"\x15" + number7z(len(attrs)) + attrs
    header += b"\0\0"
    body = b"".join(packed)
    start = struct.pack("<QQI", len(body), len(header), binascii.crc32(header))
    return b"7z\xbc\xaf\x27\x1c\0\x04" + struct.pack("<I", binascii.crc32(start)) + start + body + header


def exercise(worker, root, name, blob, expected, memory=1 << 20, cancel=False, split=False, flatten=False, limit=1 << 30):
    destination = root / name
    destination.mkdir()
    sources = []
    for i, chunk in enumerate([blob[:37], blob[37:len(blob)//2], blob[len(blob)//2:]] if split else [blob]):
        path = root / f"{name}.{i}"
        path.write_bytes(chunk)
        sources.append(path)
    request = f"7ZX1\nmemory {memory}\nlimit {limit}\nreserved .ps5-react-\ndest {destination}\n"
    if flatten: request += "flatten .exfat\n"
    request += "".join(f"source {p}\n" for p in sources) + "end\n"
    process = subprocess.Popen([str(worker)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    lines = []
    reader = threading.Thread(target=lambda: lines.extend(process.stdout.read().splitlines()))
    reader.start()
    process.stdin.write(request)
    process.stdin.flush()
    if cancel:
        process.stdin.write("cancel\n")
        process.stdin.flush()
    max_threads = 0
    deadline = time.monotonic() + 30
    while process.poll() is None and time.monotonic() < deadline:
        if os.path.isdir(f"/proc/{process.pid}/task"):
            max_threads = max(max_threads, len(os.listdir(f"/proc/{process.pid}/task")))
        time.sleep(.005)
    if process.poll() is None:
        process.kill()
        raise AssertionError("7z worker hung")
    reader.join()
    process.stdin.close()
    assert lines[0] == "ready", lines
    assert lines[-1] == ("cancelled" if cancel else "ok"), lines
    if not cancel:
        receipts = {}
        for line in lines:
            if line.startswith("f "):
                _, digest, size, path = line.split(" ", 3)
                assert path not in receipts, lines
                receipts[path] = (digest, int(size))
        assert receipts == {p: (hashlib.sha256(data).hexdigest(), len(data)) for p, data in expected}, lines
        for path, data in expected: assert (destination / path).read_bytes() == data
    return lines, max_threads


def main():
    with tempfile.TemporaryDirectory(prefix="sevenzip-threads-") as tmp:
        root = Path(tmp)
        worker = build_host(root / "rar-extract")
        # Large enough to overlap decoding, with independently generated contents.
        data = bytes(range(256)) * (4 * 1024 * 1024 // 256)
        files = [(f"dir{i}/asset.bin", data + bytes([i])) for i in range(12)]
        lines, observed = exercise(worker, root, "folders", archive7z(files), files, split=True)
        timings = next(line for line in lines if line.startswith("t "))
        assert "workers=4 " in timings, timings
        assert int(re.search(r"workers_used=(\d+)", timings)[1]) >= 2, timings
        assert int(re.search(r"active_peak=(\d+)", timings)[1]) >= 2, timings
        print("PASS independent folders:", timings, "observed OS threads=", observed)
        solid_files = [(f"solid{i}/é雪.bin", data + bytes([i])) for i in range(12)]
        lines, _ = exercise(worker, root, "solid-delta", archive7z(solid_files, solid=3, delta=True), solid_files)
        assert "workers=4 " in next(line for line in lines if line.startswith("t ")), lines
        print("PASS multi-file solid folders with Delta + LZMA2 and Unicode paths")
        if shutil.which("zstd"):
            zstd_files = [("zstandard.bin", data)]
            exercise(worker, root, "zstandard", archive7z(zstd_files, zstd=True), zstd_files)
            print("PASS third-party 7z Zstandard method compatibility")
        files = [("single.bin", data * 16)]
        lines, observed = exercise(worker, root, "reset", archive7z(files, reset_blocks=True), files)
        timings = next(line for line in lines if line.startswith("t "))
        assert "workers=1 " in timings and "decoder_threads=4 " in timings, timings
        # Alive + main + coordinator + writer is four; more proves SDK decoder
        # threads really exist, independently of our configured-thread report.
        if os.path.isdir("/proc/self/task"): assert observed > 4, observed
        print("PASS LZMA2 reset-block MT:", timings, "observed OS threads=", observed)
        exercise(worker, root, "cancel", archive7z(files, reset_blocks=True), [], cancel=True)
        small = [("folder/image.exfat", b"image")]
        exercise(worker, root, "flatten", archive7z(small), [("image.exfat", b"image")], flatten=True)
        # Failures are exercised directly, including no receipt for bad CRC.
        for name, content, kwargs in [
            ("traversal", [("../escape", b"x")], {}),
            ("collision", [("a/x.exfat", b"a"), ("b/x.exfat", b"b")], {"flatten": True}),
            ("conflict", [("file", b"a"), ("file/child", b"b")], {}),
            ("limit", [("large", data)], {"limit": 5}),
        ]:
            try: exercise(worker, root, name, archive7z(content), content, **kwargs)
            except AssertionError as error:
                assert "fail " in str(error), error
            else: raise AssertionError(f"accepted {name}")
        damaged = bytearray(archive7z([("crc.bin", data)]))
        damaged[40] ^= 1
        try: exercise(worker, root, "crc", damaged, [("crc.bin", data)])
        except AssertionError as error: assert "fail " in str(error), error
        else: raise AssertionError("accepted corrupt data")
        try: exercise(worker, root, "symlink", archive7z([("link", b"target")], attributes=[0o120777 << 16 | 0x8000]), [])
        except AssertionError as error: assert "unsafe" in str(error), error
        else: raise AssertionError("accepted symlink")
        print("PASS multipart, receipts, flattening, cancellation, CRC, path/collision and size refusal")


if __name__ == "__main__": main()
