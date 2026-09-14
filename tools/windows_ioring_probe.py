"""只读能力探测；创建空环，不提交文件操作。 / Probe an empty ring without file I/O.

Usage / 用法: python tools/windows_ioring_probe.py
Windows 11 API required; errors fail explicitly. 需要 Windows 11 API，失败显式报告。
"""

import ctypes as c
import json


class Flags(c.Structure):
    """按值传递的两个零标志。 / Two zero flags passed by value."""

    # SDK ABI: required then advisory, both UINT32. / 必需标志在前，建议标志在后。
    _fields_ = [("required", c.c_uint32), ("advisory", c.c_uint32)]


def main():
    """查询能力、创建环、验证 READ 支持并关闭。 / Query, create, check READ and close."""
    api = c.WinDLL("kernel32")
    query = api.QueryIoRingCapabilities
    query.argtypes, query.restype = [c.c_void_p], c.c_long
    caps = (c.c_uint32 * 4)()
    hr = query(c.byref(caps))
    if hr < 0:
        raise OSError(f"QueryIoRingCapabilities HRESULT={hr:#x}")
    create = api.CreateIoRing
    create.argtypes = [c.c_uint32, Flags, c.c_uint32, c.c_uint32, c.POINTER(c.c_void_p)]
    create.restype = c.c_long
    ring = c.c_void_p()
    hr = create(caps[0], Flags(0, 0), 8, 8, c.byref(ring))
    if hr < 0:
        raise OSError(f"CreateIoRing HRESULT={hr:#x}")
    close = api.CloseIoRing
    close.argtypes, close.restype = [c.c_void_p], c.c_long
    try:
        supported = api.IsIoRingOpSupported
        supported.argtypes, supported.restype = [c.c_void_p, c.c_uint32], c.c_int
        print(json.dumps(dict(max_version=caps[0], max_submission=caps[1],
                              max_completion=caps[2], flags=caps[3],
                              create_hresult=hr, read_supported=bool(supported(ring, 1)))))
    finally:
        hr = close(ring)
        if hr < 0:
            raise OSError(f"CloseIoRing HRESULT={hr:#x}")


if __name__ == "__main__":
    main()
