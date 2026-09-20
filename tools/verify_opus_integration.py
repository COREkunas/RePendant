"""Offline pinned-source/effective-build audit. No build, download or hardware.

--source-only is read-only. Full mode writes metadata and exact license into
the specified app build's opus-audit directory. It audits the static archive,
not firmware retention, runtime stack, execution speed or heap-free API usage.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

from test_opus_host import ARCHIVE, ROOT, SHA256, SOURCE, verified_sources
from test_pdm_frame_math import require

SOURCE_FILES = 482
CODEC_COMPILATION_UNITS = 132
ARCHIVE_BYTES = 10472813
LICENSE_SHA256 = "01e1167d54a096d123cf6dfbbeb19587278845c6481d2d66d545669846079551"
SILK_OVERRIDE = ROOT / "usb_firmware/src/opus_silk_fatal_override.h"
INLINE_ARM_DEFINES = {"OPUS_ARM_INLINE_ASM", "OPUS_ARM_INLINE_EDSP", "OPUS_ARM_INLINE_MEDIA"}
REQUIRED_DEFINES = {"FIXED_POINT": "1", "DISABLE_FLOAT_API": "1", "VAR_ARRAYS": "1",
    "ENABLE_ASSERTIONS": "1", "ENABLE_HARDENING": "1", "OVERRIDE_celt_fatal": "1",
    "OPUS_BUILD": "1", "DISABLE_DEBUG_FLOAT": "1", "HAVE_CONFIG_H": "1",
    "OPENPENDANT_SILK_FATAL_REDIRECT": "1", **{name: "1" for name in INLINE_ARM_DEFINES}}
FORBIDDEN_DEFINES = {"ENABLE_DRED", "ENABLE_OSCE", "ENABLE_DEEP_PLC", "OPUS_HAVE_RTCD",
    "USE_ALLOCA", "NONTHREADSAFE_PSEUDOSTACK", "CUSTOM_MODES", "FIXED_DEBUG", "FUZZING",
    "FLOAT_APPROX", "OPUS_CHECK_ASM", "DLL_EXPORT"}
REQUIRED_FLAGS = {"-fstack-protector-strong", "-fstack-usage", "-mcpu=cortex-m33", "-mthumb",
    "-mfloat-abi=soft", "-O3", "-UNDEBUG", "-fno-fast-math", "-ffunction-sections", "-fdata-sections"}
FORBIDDEN_FLAGS = {"-ffast-math", "-Ofast", "-funsafe-math-optimizations", "-fno-stack-protector", "-flto"}
REQUIRED_CONFIG = {"CONFIG_CPU_CORTEX_M33", "CONFIG_REBOOT", "CONFIG_RESET_ON_FATAL_ERROR",
    "CONFIG_ASSERT", "CONFIG_STACK_CANARIES_STRONG", "CONFIG_INIT_STACKS", "CONFIG_THREAD_STACK_INFO",
    "CONFIG_ARMV8_M_DSP"}
FORBIDDEN_CONFIG = {"CONFIG_LOG", "CONFIG_PRINTK", "CONFIG_COVERAGE", "CONFIG_COVERAGE_DUMP",
    "CONFIG_COVERAGE_DUMP_SEMIHOST", "CONFIG_SMP", "CONFIG_FPU"}
BAD_SYMBOLS = {"celt_fatal", "_silk_fatal", "abort", "fprintf", "vfprintf", "printf", "puts", "fputs",
    "__assert_func", "__assert_fail"}


def source_proof():
    require(ARCHIVE.stat().st_size == ARCHIVE_BYTES, "Pinned archive size differs")
    checked = verified_sources()
    require(checked == SOURCE_FILES, "Expected exact 482-file release")
    license_bytes = (SOURCE / "COPYING").read_bytes()
    require(hashlib.sha256(license_bytes).hexdigest() == LICENSE_SHA256, "Pinned license differs")
    return dict(source="Opus 1.6.1", verified_source_files=checked,
                archive_bytes=ARCHIVE_BYTES, archive_sha256=SHA256, license_sha256=LICENSE_SHA256)


def tokens(command):
    # CMake's Windows command strings quote paths but use ordinary unquoted GCC
    # flags. Preserve backslashes; we inspect tokens, never execute this string.
    require(isinstance(command, str) and len(command) <= 65536, "Bad compiler command")
    return [part.strip('"') for part in shlex.split(command, posix=False)]


def validate_command(command):
    parts = tokens(command)
    flags = set(parts)
    includes = [index for index, value in enumerate(parts) if value.startswith("-include")]
    require(len(includes) == 1 and parts[includes[0]] == "-include"
            and includes[0] + 1 < len(parts) and parts[includes[0] + 1] == SILK_OVERRIDE.as_posix(),
            "Exact sole target-local SILK fatal override is missing or changed")
    require(REQUIRED_FLAGS <= flags, "Missing effective fixed ARM codec flags")
    require(not FORBIDDEN_FLAGS.intersection(flags), "Forbidden compiler option")
    require(not any(flag.startswith(("-flto=", "-mfloat-abi=hard", "-mfloat-abi=softfp")) for flag in parts),
            "Unexpected ABI or LTO")
    definitions = {}
    for part in parts:
        if part.startswith("-D"):
            name, _, value = part[2:].partition("=")
            definitions[name] = value or "1"
        elif part.startswith("-U"):
            definitions.pop(part[2:], None)
    require(all(definitions.get(name) == value for name, value in REQUIRED_DEFINES.items()),
            "Required Opus macro absent or overridden")
    require(not FORBIDDEN_DEFINES.intersection(definitions) and "NDEBUG" not in definitions,
            "Forbidden Opus macro or disabled assertions")
    require(not any(name.startswith(("OPUS_ARM_", "OPUS_X86_")) and name not in INLINE_ARM_DEFINES
                    for name in definitions), "Only the three pinned inline Thumb DSP macros are reviewed")
    optimizations = [part for part in parts if re.fullmatch(r"-O(?:[0-3sgz]|fast)", part)]
    require(optimizations and optimizations[-1] == "-O3", "Effective codec optimization is not -O3")
    protectors = [part for part in parts if part.startswith("-fstack-protector")]
    require(protectors and protectors[-1] == "-fstack-protector-strong", "Strong protector overridden")
    return definitions


def validate_config(raw):
    values = dict(re.findall(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", raw, re.MULTILINE))
    require(all(values.get(name) == "y" for name in REQUIRED_CONFIG), "Required Zephyr safety config missing")
    require(not any(values.get(name) not in (None, "n") for name in FORBIDDEN_CONFIG),
            "Unexpected logging, coverage, floating point or SMP config")


def validate_archive_symbols(raw):
    rows = []
    for line in raw.splitlines():
        match = re.search(r"\s([A-Za-z?])\s+(\S+)\s*$", line)
        if match:
            rows.append((match.group(1), match.group(2)))
    require(rows, "No static archive symbols")
    require(any(kind == "U" and name == "celt_fatal" for kind, name in rows),
            "Expected assertions calling external celt_fatal")
    require(not any(name in BAD_SYMBOLS and not (name == "celt_fatal" and kind == "U")
                    for kind, name in rows), "Original fatal/default abort or stdio appears in Opus archive")
    for api in ("opus_encoder_init", "opus_encoder_get_size", "opus_encode"):
        require(any(kind in "Tt" and name == api for kind, name in rows), "Missing fixed encoder API")
    require(any(kind == "U" and name == "__stack_chk_fail" for kind, name in rows),
            "No actual stack-protector dependency in codec archive")
    require(not any(name in {"opus_encode_float", "opus_decode_float"} and kind in "Tt" for kind, name in rows),
            "Float API unexpectedly present")
    return sorted({name for kind, name in rows if kind == "U"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--nm", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    proof = source_proof()
    if args.source_only:
        require(all(value is None for value in (args.build_dir, args.library, args.nm, args.output_dir)),
                "Source-only accepts no build arguments")
        print(json.dumps(dict(result="SOURCE_VERIFIED", **proof)))
        return
    require(all(value is not None for value in (args.build_dir, args.library, args.nm, args.output_dir)),
            "Full audit requires explicit build/library/nm/output")
    build, library, output = args.build_dir.resolve(), args.library.resolve(), args.output_dir.resolve()
    require(build.is_relative_to(ROOT) and build != ROOT and library.is_relative_to(build)
            and output == build / "opus-audit", "Audit paths must stay within this workspace app build")
    require(library.is_file() and not args.library.is_symlink() and args.nm.is_file(), "Archive or nm missing")
    require(not args.output_dir.is_symlink(), "Audit output cannot be a symlink")
    commands_file, config_file = build / "compile_commands.json", build / "zephyr/.config"
    commands = json.loads(commands_file.read_text(encoding="utf-8"))
    selected = [entry for entry in commands if Path(entry["file"]).resolve().is_relative_to(SOURCE)]
    require(len(selected) == CODEC_COMPILATION_UNITS and len({entry["file"] for entry in selected}) == len(selected),
            "Missing or duplicate library compilation records")
    for entry in selected:
        relative = Path(entry["file"]).resolve().relative_to(SOURCE).as_posix()
        require(relative.endswith(".c") and not any(piece in relative.split("/")
                for piece in ("arm", "x86", "float", "dnn", "tests")), "Unreviewed Opus source family")
        validate_command(entry["command"])
    validate_config(config_file.read_text(encoding="utf-8"))
    result = subprocess.run([str(args.nm), "-A", str(library)], capture_output=True, text=True, timeout=30)
    require(result.returncode == 0, "Static library symbol inspection failed")
    unresolved = validate_archive_symbols(result.stdout)
    require(source_proof() == proof, "Pinned source changed during audit")
    output.mkdir(exist_ok=True)
    (output / "OPUS_COPYING.txt").write_bytes((SOURCE / "COPYING").read_bytes())
    (output / "compile_commands.json").write_text(json.dumps(selected, indent=2) + "\n", encoding="utf-8")
    (output / "archive-symbols.txt").write_text(result.stdout, encoding="utf-8")
    report = dict(result="PASS", **proof, compiler_commands=len(selected),
        compile_commands_sha256=hashlib.sha256(commands_file.read_bytes()).hexdigest(),
        zephyr_config_sha256=hashlib.sha256(config_file.read_bytes()).hexdigest(),
        library_bytes=library.stat().st_size, library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
        archive_unresolved_symbols=unresolved, original_fatal_and_default_abort_absent=True,
        limitations="Static archive and command/config inspection, not final ELF retention or runtime proof. "
                    "Unused create/destroy APIs may reference heap; benchmark must use caller-owned state. "
                    "C99 VLAs require measured dedicated-worker stack headroom.")
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
