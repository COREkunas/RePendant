"""Explicitly fetch pinned public source dependencies. Never accesses a device.

Existing directories/archives are verified, not overwritten. Partial failures are
left for inspection. Run with Python 3.11+ and Git installed.
"""
import hashlib
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
OPUS_SHA = '6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1'
WHISPER_REV = '927cfce34f31707e17f2bff35c349632fb9e2c3a'


def main():
    archive = ROOT / '.tools/downloads/opus-1.6.1.tar.gz'
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        request = urllib.request.Request('https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz')
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read(10472814)
        if len(data) != 10472813 or hashlib.sha256(data).hexdigest() != OPUS_SHA:
            raise SystemExit('Downloaded Opus archive failed pinned size/SHA-256')
        with archive.open('xb') as output:
            output.write(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != OPUS_SHA:
        raise SystemExit('Existing Opus archive differs; not replacing it')
    vendor = ROOT / 'third_party'
    if not (vendor / 'opus-1.6.1').exists():
        vendor.mkdir(exist_ok=True)
        with tarfile.open(archive, 'r:gz') as source:
            for entry in source.getmembers():
                target = (vendor / entry.name).resolve()
                if not (entry.isdir() or entry.isfile()) or not target.is_relative_to(vendor / 'opus-1.6.1'):
                    raise SystemExit('Rejected unsafe source archive member')
            source.extractall(vendor, filter='data')
    from test_opus_host import verified_sources
    if verified_sources() != 482:
        raise SystemExit('Unexpected Opus source count')
    whisper = ROOT / 'android_app/app/src/main/cpp/vendor/whisper.cpp'
    if not whisper.exists():
        whisper.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--depth', '1', '--branch', 'v1.9.4',
                        'https://github.com/ggml-org/whisper.cpp.git', str(whisper)], check=True)
    revision = subprocess.check_output(['git', '-C', str(whisper), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != WHISPER_REV:
        raise SystemExit('Existing whisper.cpp revision differs; not replacing it')
    subprocess.run(['git', '-C', str(whisper), 'diff', '--exit-code', 'HEAD', '--'], check=True)
    if subprocess.check_output(['git', '-C', str(whisper), 'ls-files', '--others', '--exclude-standard']):
        raise SystemExit('Unexpected untracked whisper.cpp files')
    print('Pinned Opus and whisper.cpp sources verified. No model downloaded; no device accessed.')


if __name__ == '__main__':
    main()
