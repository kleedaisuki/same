"""实测原生异步目录元数据，不修改生产路径。 / Probe native async directory metadata.

Usage / 用法:
  python tools/research_validation/async_directory_metadata.py --fixture --trials 7
  python tools/research_validation/async_directory_metadata.py --root PATH --trials 3
Creates an isolated retained fixture under build only when requested; otherwise reads
one directory without recursion. Uses events + IO_STATUS_BLOCK, not Win32 OVERLAPPED.
仅显式请求时创建并保留隔离语料；其他模式只读单目录。使用原生事件及状态块。
No hard timeout guarantee: outstanding requests are drained before buffers are freed.
不保证硬超时；释放缓冲前必须排空请求。Measurements include Python parsing overhead.
"""

import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import signal
import statistics
import struct
import sys
import tempfile
import time

# NT/Win32 constants; native information class differs from Win32 class 19.
# 原生信息类60不同于Win32信息类19；完整扩展目录记录头为88字节。
PENDING, NO_MORE = 0x103, 0x80000006
OVERLAPPED, BACKUP, OPEN_REPARSE = 0x40000000, 0x02000000, 0x00200000
DIRECTORY_CLASS, HEADER, WAIT_TIMEOUT = 60, 88, 258


class StatusBlock(c.Structure):
    """指针宽联合体加字节计数。 / Pointer-width status union followed by byte count."""

    # The first pointer-width slot contains NTSTATUS in its low 32 bits.
    # 第一指针宽槽的低32位为NTSTATUS，不以Win32 GetLastError解释。
    _fields_ = [("status", c.c_size_t), ("information", c.c_size_t)]


def api():
    """显式声明64位Windows ABI。 / Declare the 64-bit Windows ABI explicitly."""
    if os.name != "nt" or c.sizeof(c.c_void_p) != 8:
        raise RuntimeError("This experimental probe requires 64-bit Windows")
    k = c.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p,
                             w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    k.CreateEventW.argtypes = [c.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
    k.CreateEventW.restype = w.HANDLE
    k.ResetEvent.argtypes, k.ResetEvent.restype = [w.HANDLE], w.BOOL
    k.CloseHandle.argtypes, k.CloseHandle.restype = [w.HANDLE], w.BOOL
    k.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    k.WaitForSingleObject.restype = w.DWORD
    k.WaitForMultipleObjects.argtypes = [w.DWORD, c.POINTER(w.HANDLE), w.BOOL, w.DWORD]
    k.WaitForMultipleObjects.restype = w.DWORD
    k.GetFileInformationByHandleEx.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
    k.GetFileInformationByHandleEx.restype = w.BOOL
    nt = c.WinDLL("ntdll").NtQueryDirectoryFile
    nt.argtypes = [w.HANDLE, w.HANDLE, c.c_void_p, c.c_void_p, c.POINTER(StatusBlock),
                   c.c_void_p, w.ULONG, c.c_int, c.c_ubyte, c.c_void_p, c.c_ubyte]
    nt.restype = c.c_long
    return k, nt


def open_handle(k, path, asynchronous, list_directory=True):
    """打开当前路径，不跟随末级重解析点。 / Open a path without following its final reparse point."""
    handle = k.CreateFileW(str(path), 1 if list_directory else 0, 7, None, 3,
                           BACKUP | OPEN_REPARSE | (OVERLAPPED if asynchronous else 0), None)
    if handle == c.c_void_p(-1).value:
        raise c.WinError(c.get_last_error())
    return handle


def parse_records(data):
    """检查偏移与UTF-16边界，返回版本字段。 / Validate offsets/UTF-16 and return version fields."""
    records, position = {}, 0
    while position < len(data):
        if len(data) - position < HEADER:
            raise ValueError("Truncated directory header")
        nxt, _ = struct.unpack_from("<II", data, position)
        created, _, modified, changed, size, _ = struct.unpack_from("<qqqqqq", data, position + 8)
        attributes, length = struct.unpack_from("<II", data, position + 56)
        end = position + HEADER + length
        if not length or length % 2 or end > len(data):
            raise ValueError("Invalid directory filename length")
        if nxt and (nxt % 8 or nxt < HEADER + length or position + nxt >= len(data)):
            raise ValueError("Invalid NextEntryOffset")
        name = data[position + HEADER:end].decode("utf-16-le", errors="surrogatepass")
        if name not in (".", ".."):
            if name in records:
                raise ValueError("Duplicate directory entry")
            # Directory EOF is not file content size and differs across information classes.
            # 目录EOF不是文件内容大小，不同信息类可不同；目录不比较该字段。
            records[name] = (None if attributes & 0x10 else size, created, modified, changed, attributes,
                             data[position + 72:position + 88].hex())
        if not nxt:
            return records
        position += nxt
    return records


class Cursor:
    """一个目录至多一个在途请求。 / At most one outstanding request per directory cursor."""

    def __init__(self, k, nt, path, asynchronous, buffer_bytes):
        """分配稳定缓冲与状态块，独占句柄。 / Own handles and allocate stable request storage."""
        self.k, self.nt = k, nt
        self.path, self.asynchronous = path, asynchronous
        # Allocate before opening handles so allocation failure cannot leak handles.
        # 先分配后打开句柄，避免分配失败泄漏句柄。
        self.buffer, self.iosb = c.create_string_buffer(buffer_bytes), StatusBlock()
        self.handle = open_handle(k, path, asynchronous)
        self.event = k.CreateEventW(None, True, False, None)
        if not self.event:
            error = c.get_last_error()
            k.CloseHandle(self.handle)
            raise c.WinError(error)
        # Storage lives until the request reaches a terminal state. / 存储活到请求终态。
        self.pending, self.done, self.first = False, False, True
        self.records = {}
        self.calls, self.pending_calls, self.not_ready_on_return = 0, 0, 0
        self.call_ms, self.max_call_ms = 0.0, 0.0

    def submit(self):
        """提交一次续枚举；不立即等待异步完成。 / Submit one continuation without waiting."""
        if self.pending or self.done:
            raise RuntimeError("Invalid cursor submission state")
        if not self.k.ResetEvent(self.event):
            raise c.WinError(c.get_last_error())
        self.iosb.status, self.iosb.information = PENDING, 0
        start = time.perf_counter()
        status = self.nt(self.handle, self.event if self.asynchronous else None, None, None,
                         c.byref(self.iosb), self.buffer, len(self.buffer), DIRECTORY_CLASS,
                         0, None, self.first) & 0xFFFFFFFF
        elapsed = (time.perf_counter() - start) * 1000
        self.calls += 1
        self.call_ms += elapsed
        self.max_call_ms = max(self.max_call_ms, elapsed)
        self.first = False
        if status == PENDING:
            self.pending = True
            self.pending_calls += 1
            wait_handle = self.event if self.asynchronous else self.handle
            self.not_ready_on_return += self.k.WaitForSingleObject(wait_handle, 0) == WAIT_TIMEOUT
        else:
            self.consume(status)

    def consume(self, status):
        """只在终态读取结果，EOF与错误分开。 / Consume terminal results, distinguishing EOF/errors."""
        if status == NO_MORE:
            self.done = True
            return
        if status != 0:
            raise RuntimeError(f"NtQueryDirectoryFile NTSTATUS=0x{status:08x}")
        count = self.iosb.information
        if not 0 < count <= len(self.buffer):
            raise RuntimeError("Empty/oversized successful response; probe does not resize buffers")
        records = parse_records(self.buffer.raw[:count])
        if self.records.keys() & records.keys():
            raise RuntimeError("Duplicate entry across batches")
        self.records.update(records)

    def complete(self):
        """确认终态才可重用请求存储。 / Reuse request storage only after terminal completion."""
        wait_handle = self.event if self.asynchronous else self.handle
        result = self.k.WaitForSingleObject(wait_handle, 0xFFFFFFFF)
        if result != 0:
            raise c.WinError(c.get_last_error())
        self.pending = False
        self.consume(self.iosb.status & 0xFFFFFFFF)

    def close(self):
        """释放前排空；无不安全超时释放。 / Drain before releasing, with no unsafe timeout free."""
        if self.pending:
            wait_handle = self.event if self.asynchronous else self.handle
            if self.k.WaitForSingleObject(wait_handle, 0xFFFFFFFF) != 0:
                # Cannot prove safety: terminate this isolated process, do not free live storage.
                # 无法证明安全：终止隔离实验进程，不释放仍在使用的Python存储。
                os._exit(3)
            self.pending = False
        self.k.CloseHandle(self.event)
        self.k.CloseHandle(self.handle)


def scan(k, nt, paths, asynchronous, depth, buffer_bytes):
    """单线程驱动多目录，所有目录等量查询。 / Drive bounded directory requests on one thread."""
    start = time.perf_counter()
    all_records, totals = {}, dict(calls=0, pending_calls=0, not_ready_on_return=0, call_ms=0.0)
    max_call_ms, peak_pending = 0.0, 0
    for offset in range(0, len(paths), depth):
        cursors = []
        try:
            for path in paths[offset:offset + depth]:
                cursors.append(Cursor(k, nt, path, asynchronous, buffer_bytes))
            while any(not cursor.done for cursor in cursors):
                for cursor in cursors:
                    if not cursor.done and not cursor.pending:
                        cursor.submit()
                pending = [cursor for cursor in cursors if cursor.pending]
                peak_pending = max(peak_pending, len(pending))
                if pending:
                    handles = (w.HANDLE * len(pending))(*[
                        cursor.event if asynchronous else cursor.handle for cursor in pending])
                    result = k.WaitForMultipleObjects(len(pending), handles, False, 0xFFFFFFFF)
                    if result >= len(pending):
                        raise c.WinError(c.get_last_error())
                    pending[result].complete()
            for cursor in cursors:
                all_records[cursor.path] = cursor.records
                for key in totals:
                    totals[key] += getattr(cursor, key)
                max_call_ms = max(max_call_ms, cursor.max_call_ms)
        finally:
            for cursor in cursors:
                cursor.close()
    elapsed = (time.perf_counter() - start) * 1000
    return all_records, dict(ms=elapsed, peak_pending=peak_pending,
                             max_call_ms=max_call_ms, **totals)


def oracle(k, paths):
    """用独立Win32逐句柄查询核验，非目录时间戳推断。 / Verify via independent per-handle Win32 queries."""
    result, count = {}, 0
    for path in paths:
        entries = {}
        for name in os.listdir(path):
            handle = open_handle(k, path / name, False, list_directory=False)
            try:
                basic, standard, identity = (c.create_string_buffer(size) for size in (40, 24, 24))
                for info, buffer in ((0, basic), (1, standard), (18, identity)):
                    if not k.GetFileInformationByHandleEx(handle, info, buffer, len(buffer)):
                        raise c.WinError(c.get_last_error())
                created, _, modified, changed, attributes = struct.unpack_from("<qqqqI", basic.raw)
                size = struct.unpack_from("<q", standard.raw, 8)[0]
                entries[name] = (None if attributes & 0x10 else size, created, modified, changed,
                                 attributes, identity.raw[8:24].hex())
                count += 1
            finally:
                k.CloseHandle(handle)
        result[path] = entries
    return result, count


def fixture():
    """只创建新的隔离目录，不清理或覆盖已有数据。 / Create a fresh retained fixture, never overwrite."""
    parent = Path(__file__).resolve().parents[2] / "build" / "async-metadata"
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="fixture-", dir=parent))
    paths = []
    for directory in range(16):
        path = root / f"dir-{directory:02d}"
        path.mkdir()
        paths.append(path)
        for index in range(256):
            name = f"file-{index:04d}-测试.bin"
            (path / name).write_bytes(bytes([index % 256]) * ((index * 137) % 4096))
        os.link(path / "file-0001-测试.bin", path / "hardlink.bin")
    empty = root / "empty"
    empty.mkdir()
    paths.extend((empty, root))
    print(f"Retained isolated fixture: {root}", file=sys.stderr)
    return paths


def main():
    """先核验再交替测量，同步与异步保持相同信息类。 / Validate then pair identical sync/async queries."""
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--fixture", action="store_true")
    source.add_argument("--root", type=Path)
    parser.add_argument("--trials", type=int, default=7, choices=range(1, 21))
    parser.add_argument("--buffer", type=int, default=65536, choices=(4096, 65536))
    args = parser.parse_args()
    k, nt = api()
    paths = fixture() if args.fixture else [args.root.resolve(strict=True)]
    # Observe the first pass before the oracle warms file metadata, without claiming cold cache.
    # 在逐句柄oracle预热前观察首轮，但不把它称为冷缓存。
    first_records, first_timing = scan(k, nt, paths, True, 16, args.buffer)
    oracle_start = time.perf_counter()
    expected, checked = oracle(k, paths)
    oracle_ms = (time.perf_counter() - oracle_start) * 1000
    if first_records != expected:
        raise RuntimeError("First enumeration differs from per-handle oracle")
    modes = (("sync", False, 1), ("async_depth1", True, 1), ("async_depth16", True, 16))
    print(json.dumps(dict(kind="setup", directories=len(paths), entries=checked,
                          buffer_bytes=args.buffer, oracle="Win32 per-handle Basic/Standard/Id",
                          oracle_ms=oracle_ms, directory_size_compared=False)))
    print(json.dumps(dict(kind="first_async", verified=True, **first_timing)))
    samples = {name: [] for name, _, _ in modes}
    for trial in range(-1, args.trials):
        for name, asynchronous, depth in (modes if trial % 2 == 0 else modes[::-1]):
            records, timing = scan(k, nt, paths, asynchronous, depth, args.buffer)
            if records != expected:
                raise RuntimeError("Directory metadata differs from fresh per-handle oracle")
            if trial >= 0:
                samples[name].append(timing["ms"])
                print(json.dumps(dict(kind="measurement", backend=name, trial=trial,
                                      entries=checked, verified=True, **timing)), flush=True)
    final, _ = oracle(k, paths)
    if final != expected:
        raise RuntimeError("Directory changed during experiment")
    print(json.dumps(dict(kind="summary", median_ms={key: statistics.median(value)
                                                     for key, value in samples.items()},
                          metadata_verified=True, final_oracle_verified=True)))


if __name__ == "__main__":
    # A KeyboardInterrupt between native submission and state publication is unsafe.
    # 原生提交返回与pending状态发布之间不可被KeyboardInterrupt打断；所有请求排空后恢复。
    previous = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        main()
    finally:
        signal.signal(signal.SIGINT, previous)
