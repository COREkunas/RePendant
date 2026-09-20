"""Compile and test the firmware's pure PDM frame arithmetic offline.

Uses the already installed Microsoft C compiler, a temporary native DLL, and
Python's arbitrary-precision integer reference. No serial/Bluetooth/debugger
modules, device operations, package installation, or firmware writes exist.
Generated compiler outputs stay in a validated, uniquely named temporary folder.
"""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "usb_firmware/src/mic_frame_math.h"
WRAPPER = ROOT / "tools/test_pdm_frame_math_native.c"
SAMPLES = 320
SUM_MIN = -10_485_760
SUM_MAX = 10_485_440
SQUARE_MAX = 343_597_383_680
FRAME_BOUND = SAMPLES * SQUARE_MAX
SENTINEL = 0xA55AC33C12345678


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(arguments: list[str], cwd: Path) -> str:
    result = subprocess.run(arguments, cwd=cwd, text=True, capture_output=True,
                            timeout=60, check=False)
    require(result.returncode == 0,
            f"Offline compiler failed ({result.returncode}):\n"
            f"{result.stdout}\n{result.stderr}")
    return result.stdout.strip()


def compiler_paths() -> tuple[Path, Path, Path]:
    require(os.name == "nt", "This offline native test requires Windows/MSVC")
    vswhere = Path("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
    require(vswhere.is_file(), "Existing Visual Studio discovery tool is unavailable")
    found = run([str(vswhere), "-latest", "-products", "*", "-requires",
                 "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-find",
                 "VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"], ROOT).splitlines()
    require(len(found) == 1, "Expected exactly one installed x64 MSVC compiler")
    compiler = Path(found[0]).resolve()
    linker = compiler.with_name("link.exe")
    include = compiler.parents[3] / "include"
    require(compiler.is_file() and linker.is_file() and (include / "stdint.h").is_file(),
            "Installed MSVC compiler/linker/standard headers are incomplete")
    return compiler, linker, include


def reference(total: int, square: int) -> int | None:
    if not SUM_MIN <= total <= SUM_MAX or not 0 <= square <= SQUARE_MAX:
        return None
    numerator = SAMPLES * square - total * total
    return numerator if numerator >= 0 else None


def exercise_native(function) -> dict[str, int]:
    counts = {"valid_sample_frames": 0, "guard_and_moment_cases": 0,
              "window_reconciliations": 0, "null_output_checks": 0}

    def check(total: int, square: int) -> int | None:
        expected = reference(total, square)
        output = ctypes.c_uint64(SENTINEL)
        result = function(total, square, ctypes.byref(output))
        require(result == int(expected is not None),
                f"Native validity mismatch for total={total}, square={square}")
        require(output.value == (SENTINEL if expected is None else expected),
                f"Native result/output-preservation mismatch for {total}, {square}")
        return expected

    def samples_case(samples: list[int]) -> int:
        require(len(samples) == SAMPLES and all(-32768 <= x <= 32767 for x in samples),
                "Invalid offline synthetic int16 frame")
        result = check(sum(samples), sum(x * x for x in samples))
        require(result is not None, "A valid sample frame was rejected")
        counts["valid_sample_frames"] += 1
        return result

    for value in (-32768, -32767, -1, 0, 1, 32766, 32767):
        require(samples_case([value] * SAMPLES) == 0, "Constant DC produced AC energy")
    patterns = (
        [-32768, 32767], [-1, 1], [32766, 32767], [-32768, -32767],
        [-32768] + [0] * 319, [32767] + [0] * 319,
        list(range(-160, 160)),
    )
    for pattern in patterns:
        samples_case([pattern[i % len(pattern)] for i in range(SAMPLES)])
    require(samples_case([-32768, 32767] * 160) == 109_947_807_360_000,
            "Full-scale alternating frame numerator is incorrect")
    require(samples_case([32766, 32767] * 160) == 25_600,
            "Small variation on large DC lost integer precision")

    rng = random.Random(20260915)
    frames = []
    for index in range(1000):
        if index % 4 == 0:
            samples = [rng.randint(-32768, 32767) for _ in range(SAMPLES)]
        elif index % 4 == 1:
            samples = [rng.choice((-32768, -1, 0, 0, 0, 1, 32767))
                       for _ in range(SAMPLES)]
        elif index % 4 == 2:
            base = rng.randint(-32768, 32766)
            samples = [base + rng.randrange(2) for _ in range(SAMPLES)]
        else:
            samples = [rng.randint(-32768, 32767)] * SAMPLES
        frames.append((samples, samples_case(samples)))

    totals = (-(1 << 63), SUM_MIN - 1, SUM_MIN, SUM_MIN + 1, -1, 0, 1,
              SUM_MAX - 1, SUM_MAX, SUM_MAX + 1, (1 << 63) - 1)
    squares = (0, 1, SQUARE_MAX - 1, SQUARE_MAX, SQUARE_MAX + 1, (1 << 64) - 1)
    for total in totals:
        for square in squares:
            check(total, square)
            counts["guard_and_moment_cases"] += 1
    for _ in range(2000):
        total = rng.randint(SUM_MIN - 20, SUM_MAX + 20)
        threshold = (total * total + SAMPLES - 1) // SAMPLES
        square = max(0, threshold + rng.choice((-1, 0, 1, 10)))
        check(total, square)
        counts["guard_and_moment_cases"] += 1

    for block_count in (1, 2, 24, 25):
        selected = frames[:block_count]
        numerator_sum = sum(item[1] for item in selected)
        maximum = max(item[1] for item in selected)
        sample_count = block_count * SAMPLES
        require(numerator_sum <= block_count * FRAME_BOUND < (1 << 52),
                "Window accumulation bound failed")
        require(maximum <= numerator_sum <= block_count * maximum,
                "Window sum/maximum reconciliation failed")
        reference_sum = sum(SAMPLES * sum(x * x for x in values) - sum(values) ** 2
                            for values, _ in selected)
        require(numerator_sum == reference_sum and sample_count == block_count * 320,
                "Per-frame window arithmetic reconciliation failed")
        counts["window_reconciliations"] += 1
    for total, square in ((0, 0), (SUM_MIN, SQUARE_MAX), ((1 << 63) - 1, 0)):
        require(function(total, square, None) == 0, "NULL output must be rejected")
        counts["null_output_checks"] += 1
    return counts


def main() -> None:
    compiler, linker, include = compiler_paths()
    require(HEADER.is_file() and WRAPPER.is_file(), "Shared header/test wrapper missing")
    header_sha = hashlib.sha256(HEADER.read_bytes()).hexdigest()
    temp_root = Path(tempfile.gettempdir()).resolve()
    with tempfile.TemporaryDirectory(prefix="openpendant-pdm-math-") as directory:
        build = Path(directory).resolve()
        require(build.parent == temp_root and build.name.startswith("openpendant-pdm-math-"),
                "Unexpected generated temporary build location")
        obj, dll = build / "frame_math.obj", build / "frame_math.dll"
        run([str(compiler), "/nologo", "/c", "/TC", "/std:c11", "/O2", "/W4", "/WX",
             "/GS-", "/Zl", f"/I{include}", f"/Fo{obj}", str(WRAPPER)], build)
        run([str(linker), "/NOLOGO", "/DLL", "/NOENTRY", "/NODEFAULTLIB", "/MACHINE:X64",
             f"/OUT:{dll}", str(obj)], build)
        library = ctypes.CDLL(str(dll))
        try:
            function = library.pdm_frame_math_test
            function.argtypes = [ctypes.c_int64, ctypes.c_uint64,
                                 ctypes.POINTER(ctypes.c_uint64)]
            function.restype = ctypes.c_int
            counts = exercise_native(function)
            require(hashlib.sha256(HEADER.read_bytes()).hexdigest() == header_sha,
                    "Shared firmware header changed during native testing")
        finally:
            # Release our own temporary DLL before TemporaryDirectory removes it.
            import _ctypes
            _ctypes.FreeLibrary(library._handle)
            library._handle = 0
        print(json.dumps({"success": True, "scope": "offline native shared-C arithmetic only",
                          "compiler": str(compiler), "header_sha256": header_sha,
                          "tests": counts, "total_tests": sum(counts.values())}, indent=2))


if __name__ == "__main__":
    main()
