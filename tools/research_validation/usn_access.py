"""只读 USN 探针，不保存名称或修改卷。 / Read-only USN probe; no names saved or volume mutations.

Usage / 用法: python tools/research_validation/usn_access.py --root X:\\
Requires Windows; queries then performs exactly one bounded, non-waiting
unprivileged read if the query succeeds. / 需要 Windows；查询成功后仅一次有界非等待读取。
"""

import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
import re
import struct

# Windows SDK winioctl.h: FILE_DEVICE_FILE_SYSTEM, FILE_ANY_ACCESS.
# Windows SDK 控制码；前者 METHOD_BUFFERED，后者 METHOD_NEITHER。
FSCTL_QUERY_USN_JOURNAL = 0x900F4
FSCTL_READ_UNPRIVILEGED_USN_JOURNAL = 0x903AB


def volume_root(value):
    """仅接受显式盘符根目录。 / Accept only an explicit drive-letter volume root."""
    if re.fullmatch(r"[A-Za-z]:[\\/]", value) is None:
        raise argparse.ArgumentTypeError("Expected a drive root such as X:\\; no subpaths or UNC")
    return value[0].upper() + ":\\"


def summarize_records(data):
    """有界检查 V2/V3 记录，不解码名称。 / Bounds-check V2/V3 records without decoding names."""
    if len(data) < 8:
        raise ValueError("Missing USN continuation header")
    position, records, named = 8, 0, 0
    versions = set()
    while position < len(data):
        if len(data) - position < 8:
            raise ValueError("Truncated record header")
        length, major, _ = struct.unpack_from("<IHH", data, position)
        if length < 8 or length % 8 or length > len(data) - position:
            raise ValueError("Invalid record length")
        minimum = {2: 60, 3: 76}.get(major)
        if minimum is None:
            raise ValueError(f"Unsupported record version: {major}")
        if length < minimum:
            raise ValueError("Truncated version-specific record")
        name_length, name_offset = struct.unpack_from("<HH", data, position + minimum - 4)
        if name_length % 2 or name_offset % 2:
            raise ValueError("Unaligned UTF-16 name field")
        if name_length and (name_offset < minimum or name_offset + name_length > length):
            raise ValueError("Name field outside record")
        named += bool(name_length)
        versions.add(major)
        records += 1
        position += length
    return dict(record_count=records, versions=sorted(versions), records_with_name=named)


def kernel_api():
    """声明 Windows ABI，避免句柄截断。 / Declare Windows ABI to avoid handle truncation."""
    kernel = c.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p,
                                  w.DWORD, w.DWORD, w.HANDLE]
    kernel.CreateFileW.restype = w.HANDLE
    kernel.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, c.c_void_p, w.DWORD,
                                      c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
    kernel.DeviceIoControl.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.CloseHandle.restype = w.BOOL
    return kernel


def self_test():
    """离线测试输入边界，不调用 Windows。 / Test parser boundaries offline without Windows calls."""
    checks = 0
    for root in ("X:", "X:\\sub", "\\\\server\\share", "../", "X:\\\\"):
        try:
            volume_root(root)
        except argparse.ArgumentTypeError:
            checks += 1
            continue
        raise RuntimeError("Invalid root accepted")
    invalid = [b"", b"\0" * 9]
    invalid += [b"\0" * 8 + struct.pack("<IHH", 8, major, 0) for major in (2, 3, 4)]
    for data in invalid:
        try:
            summarize_records(data)
        except ValueError:
            checks += 1
            continue
        raise RuntimeError("Invalid record accepted")
    for major, size in ((2, 64), (3, 80)):
        record = bytearray(size)
        struct.pack_into("<IHH", record, 0, size, major, 0)
        if summarize_records(b"\0" * 8 + record)["record_count"] != 1:
            raise RuntimeError("Valid record rejected")
        checks += 1
    print(json.dumps(dict(parser_validation_checks=checks, passed=True)))


def probe(root):
    """查询日志并只读一批，失败显式退出。 / Query journal and read one batch, failing explicitly."""
    kernel = kernel_api()
    # Root-directory handle, no desired access, backup semantics without privilege changes.
    # 根目录句柄，无所需访问权限；使用目录打开标志但不调整进程权限。
    handle = kernel.CreateFileW(root, 0, 7, None, 3, 0x02000000, None)
    if handle == c.c_void_p(-1).value:
        raise c.WinError(c.get_last_error())
    try:
        output, count = c.create_string_buffer(65536), w.DWORD()
        ok = kernel.DeviceIoControl(handle, FSCTL_QUERY_USN_JOURNAL, None, 0, output, len(output),
                                    c.byref(count), None)
        if not ok:
            raise c.WinError(c.get_last_error())
        if not 56 <= count.value <= len(output):
            raise ValueError("Invalid QUERY_USN_JOURNAL response size")
        print(json.dumps(dict(query_ok=True, bytes=count.value)))
        journal_id, first, _ = struct.unpack_from("<Qqq", output.raw)
        # V0 input; zero timeout/bytes-to-wait avoids waiting for future changes.
        # V0 输入；零超时/等待字节数，不等待后续变更；缓冲固定为 64 KiB。
        request = c.create_string_buffer(struct.pack("<qIIQQQ", first, 0xFFFFFFFF,
                                                     0, 0, 0, journal_id))
        count.value = 0
        ok = kernel.DeviceIoControl(handle, FSCTL_READ_UNPRIVILEGED_USN_JOURNAL,
                                    request, 40, output, len(output),
                                    c.byref(count), None)
        if not ok:
            raise c.WinError(c.get_last_error())
        if count.value > len(output):
            raise ValueError("Read response exceeds buffer")
        summary = summarize_records(output.raw[:count.value])
        print(json.dumps(dict(unprivileged_read_ok=True, bytes=count.value, **summary)))
    finally:
        if not kernel.CloseHandle(handle):
            raise c.WinError(c.get_last_error())


def main():
    """要求显式根目录，不推断用户卷。 / Require an explicit root, never infer a user volume."""
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--root", type=volume_root)
    mode.add_argument("--self-test", action="store_true", help="Offline parser checks / 离线边界检查")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if os.name != "nt":
        parser.error("Windows is required")
    probe(args.root)


if __name__ == "__main__":
    main()
