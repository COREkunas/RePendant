"""Verify pinned Opus sources and evaluate two PC builds with generated PCM only.

No source download, firmware build, installation or hardware/audio-file access.
Outputs stay in a new workspace directory. Host results do not measure nRF CPU,
stack, energy, speech quality or durable recording behavior.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import time

from test_pdm_frame_math import ROOT, require

ARCHIVE = ROOT / ".tools/downloads/opus-1.6.1.tar.gz"
SHA256 = "6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
SOURCE = ROOT / "third_party/opus-1.6.1"
CMAKE = Path("C:/ncs/toolchains/dcbdc366a1/opt/bin/cmake.exe")
CTEST = CMAKE.with_name("ctest.exe")
EXPECTED_TESTS = (
    "openpendant_opus_synthetic", "test_opus_decode", "test_opus_padding",
    "test_opus_api", "test_opus_encode", "test_opus_extensions",
)


def validate_test_listing(listing):
    names = re.findall(r"^\s*Test #\d+: (\S+)\s*$", listing, flags=re.MULTILINE)
    totals = re.findall(r"^Total Tests: (\d+)\s*$", listing, flags=re.MULTILINE)
    require(len(names) == len(EXPECTED_TESTS) and set(names) == set(EXPECTED_TESTS)
            and totals == [str(len(EXPECTED_TESTS))],
            "Expected the exact six registered Opus host tests")
    return names


def validate_synthetic_results(results):
    rows = [json.loads(line) for line in results.splitlines() if line.startswith("{")]
    scenarios = [(bitrate, complexity) for bitrate in (16000, 24000)
                 for complexity in (0, 5, 10)]
    fields = {"bitrate", "complexity", "frames", "frame_samples", "packet_min",
              "packet_max", "packet_bytes_total", "lookahead_at_16khz",
              "encoder_state_bytes_host", "decoder_state_bytes_host"}
    require(len(rows) == len(scenarios), "Missing generated-PCM codec scenarios")
    for row, (bitrate, complexity) in zip(rows, scenarios):
        require(isinstance(row, dict) and set(row) == fields
                and all(type(value) is int for value in row.values()),
                "Unexpected synthetic codec result schema")
        expected = dict(bitrate=bitrate, complexity=complexity, frames=300,
                        frame_samples=320, packet_min=bitrate // 400,
                        packet_max=bitrate // 400,
                        packet_bytes_total=300 * (bitrate // 400))
        require(all(row[key] == value for key, value in expected.items()),
                "Synthetic codec scenario, frame count or CBR size differs")
        require(0 <= row["lookahead_at_16khz"] < 16000
                and row["encoder_state_bytes_host"] > 0
                and row["decoder_state_bytes_host"] > 0,
                "Invalid synthetic codec lookahead or host state size")
    return rows


def verified_sources():
    require(ARCHIVE.is_file() and not ARCHIVE.is_symlink(), "Pinned archive missing")
    require(hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() == SHA256,
            "Official source archive checksum mismatch")
    checked = 0
    expected = set()
    with tarfile.open(ARCHIVE, "r:gz") as archive:
        for member in archive:
            require(member.isdir() or member.isfile(), "Source archive has special entry")
            path = ROOT / "third_party" / member.name
            require(path.resolve().is_relative_to(SOURCE.resolve()), "Unsafe source path")
            require(not path.is_symlink(), "Source symlink is not accepted")
            if not member.isfile():
                continue
            expected.add(path.resolve())
            stream = archive.extractfile(member)
            require(stream is not None and path.is_file(), "Source member missing")
            require(hashlib.sha256(stream.read()).digest() ==
                    hashlib.sha256(path.read_bytes()).digest(),
                    f"Vendor source differs from release: {member.name}")
            checked += 1
    actual = set()
    for path in SOURCE.rglob("*"):
        require(not path.is_symlink(), "Vendor tree contains a symlink")
        if path.is_file():
            actual.add(path.resolve())
    require(actual == expected, "Vendor tree contains missing or extra files")
    return checked


def run(arguments, output, log_name, timeout=60):
    environment = os.environ.copy()
    for name in ("CL", "_CL_", "CFLAGS", "CPPFLAGS", "CXXFLAGS", "LDFLAGS"):
        environment.pop(name, None)
    environment.pop("TEST_OPUS_NOFUZZ", None)
    environment["SEED"] = "20260915"
    started = time.monotonic()
    try:
        result = subprocess.run([str(arg) for arg in arguments], cwd=ROOT,
                                capture_output=True, timeout=timeout, env=environment)
    except subprocess.TimeoutExpired as error:
        log = ((error.stdout or b"") + (error.stderr or b"")).decode("utf-8", "replace")
        (output / log_name).write_text(log + f"\nTIMEOUT after {timeout}s\n", encoding="utf-8")
        raise
    log = (result.stdout + result.stderr).decode("utf-8", "replace")
    (output / log_name).write_text(log, encoding="utf-8")
    require(result.returncode == 0,
            f"Offline Opus command failed ({result.returncode}), see {output / log_name}:\n{log[-2500:]}")
    return log, round(time.monotonic() - started, 3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    require(output.is_relative_to(ROOT / "codec_evaluation") and not output.exists(),
            "Use a new directory within codec_evaluation; no overwrite")
    require(CMAKE.is_file() and CTEST.is_file(), "Installed CMake/CTest missing")
    checked = verified_sources()
    output.mkdir(parents=True)
    report = dict(source="Opus 1.6.1", archive_sha256=SHA256,
                  verified_source_files=checked, hardware_access=False,
                  private_audio_access=False, firmware_integration=False,
                  seed=20260915, upstream_test_timeout_seconds=300,
                  configurations=[], success=False,
                  limitations="PC generic-C synthetic tests; not nRF CPU/stack/energy, "
                              "speech quality, container, storage or sync proof")
    try:
        for mode in ("float", "fixed"):
            build = output / mode
            _, config_s = run([CMAKE, "-S", ROOT / "tools/opus_eval", "-B", build,
                              "-G", "Visual Studio 17 2022", "-A", "x64",
                              f"-DOPUS_SOURCE_DIR={SOURCE.as_posix()}",
                              f"-DOPUS_FIXED_POINT={'ON' if mode == 'fixed' else 'OFF'}"],
                             output, f"{mode}-configure.log")
            _, build_s = run([CMAKE, "--build", build, "--config", "Release", "--parallel", "4"],
                            output, f"{mode}-build.log")
            listing, _ = run([CTEST, "--test-dir", build, "-C", "Release", "-N"],
                             output, f"{mode}-test-list.log")
            test_names = validate_test_listing(listing)
            _, test_s = run([CTEST, "--test-dir", build, "-C", "Release", "--timeout", "300",
                            "--output-on-failure", "--parallel", "4"],
                           output, f"{mode}-tests.log", timeout=660)
            results, _ = run([build / "Release/openpendant_opus_synthetic.exe"],
                             output, f"{mode}-synthetic.log")
            rows = validate_synthetic_results(results)
            item = dict(mode=mode, configured_seconds=config_s, built_seconds=build_s,
                        test_seconds=test_s, registered_tests=test_names,
                        synthetic_scenarios=rows, success=True)
            report["configurations"].append(item)
            print(json.dumps(item), flush=True)
        require(verified_sources() == checked, "Vendor sources changed during evaluation")
        report["success"] = True
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS; no hardware/audio-file access. Report: {output / 'report.json'}", flush=True)


if __name__ == "__main__":
    main()
