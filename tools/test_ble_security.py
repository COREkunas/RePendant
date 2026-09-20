"""Offline tests of actual ble_security.c against deterministic native fakes.

No Bluetooth device, real pairing, keys, signing or flash access. Exercises
admission, expiry/completion interleavings, ref ownership and secret-output
boundaries. The fakes do not prove SMP cryptography, NVS durability, real RTOS
scheduling or over-the-air interoperability; those need independent checks.
"""
from __future__ import annotations
import ctypes
import hashlib
import json
from pathlib import Path
import tempfile
from test_pdm_frame_math import ROOT, compiler_paths, require, run


def main():
    compiler, linker, include = compiler_paths()
    standard = sorted(Path("C:/Program Files (x86)/Windows Kits/10/Include").glob("*/ucrt"))
    require(bool(standard), "Existing Windows SDK C headers unavailable")
    stubs = ROOT / "tools/tests/ble_security"
    production = ROOT / "usb_firmware/src/ble_security.c"
    temp_root = Path(tempfile.gettempdir()).resolve()
    with tempfile.TemporaryDirectory(prefix="openpendant-ble-security-") as directory:
        build = Path(directory).resolve()
        require(build.parent == temp_root and build.name.startswith("openpendant-ble-security-"),
                "Unexpected temporary build path")
        obj, dll = build / "security.obj", build / "security.dll"
        run([str(compiler), "/nologo", "/c", "/TC", "/std:c11", "/O2", "/W4", "/WX",
             "/GS-", "/Zl", f"/I{include}", f"/I{standard[-1]}", f"/I{stubs}",
             f"/Fo{obj}", str(stubs / "native.c")], build)
        run([str(linker), "/NOLOGO", "/DLL", "/NOENTRY", "/NODEFAULTLIB", "/MACHINE:X64",
             f"/OUT:{dll}", str(obj)], build)
        lib = ctypes.CDLL(str(dll))
        lib.security_tests.restype = ctypes.c_int
        lib.security_assertion_count.restype = ctypes.c_int
        result = lib.security_tests()
        count = lib.security_assertion_count()
        # Windows cannot remove a loaded DLL; release the private test library.
        ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary.restype = ctypes.c_int
        require(ctypes.windll.kernel32.FreeLibrary(lib._handle) != 0, "Test DLL unload failed")
        require(result == 0, f"Actual security C failed at native.c:{result} after {count} assertions")
        print(json.dumps({"result": "PASS", "actual_c_assertions": count,
                          "source_sha256": hashlib.sha256(production.read_bytes()).hexdigest(),
                          "hardware_access": False,
                          "limitations": "Host fakes; no cryptography, NVS durability, real scheduler or radio proof"}, indent=2))


if __name__ == "__main__":
    main()
