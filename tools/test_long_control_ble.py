"""Actual long-control broker/core with generated metadata and a fake radio only."""
import ctypes as C
import hashlib
import json
from pathlib import Path
import sys
import subprocess
import tempfile
from test_pdm_frame_math import ROOT, compiler_paths, require, run

def worker(path):
    require(path.name == 'long.dll' and path.parent.name.startswith('pendant-long-control-'), 'Unexpected DLL')
    dll = C.CDLL(str(path)); out = (C.c_uint64 * 3)()
    dll.long_control_ble_tests.argtypes = [C.POINTER(C.c_uint64)]
    require(dll.long_control_ble_tests(out) == 0, 'Native checks')
    print(json.dumps(dict(result='PASS', assertions=out[0], groups=out[1], context_bytes=out[2])))

def main(portable=False):
    stub = ROOT / 'tools/tests/recording_ble'
    src = ROOT / 'usb_firmware/src'
    sources = [ROOT/'tools/tests/long_control_ble_native.c', src/'recording_control.c', src/'long_recording_control_codec.c']
    paths = [*sources, *[src/name for name in ('recording_control.h','recording_control_ble.c','recording_control_ble.h',
        'recording_long_runtime.h','recording_control_worker.h','long_recording_control_codec.h')], *stub.rglob('*.h'), Path(__file__).resolve()]
    before = {p: p.read_bytes() for p in paths}
    cc, ld, inc = compiler_paths(); sdk = Path('C:/Program Files (x86)/Windows Kits/10')
    headers = sorted((sdk/'Include').glob('*/ucrt'))[-1]
    libs = [inc.parent/'lib/x64', sdk/'Lib'/headers.parent.name/'ucrt/x64', sdk/'Lib'/headers.parent.name/'um/x64']
    with tempfile.TemporaryDirectory(prefix='pendant-long-control-') as folder:
        build = Path(folder); objects = []
        for i, source in enumerate(sources):
            obj = build/f'{i}.obj'; objects.append(obj)
            run([cc,'/nologo','/c','/TC','/std:c11','/experimental:c11atomics','/O1','/MD','/W4','/WX',
                 *(['/DOPENPENDANT_PORTABLE_RECORDING=1'] if portable else []),
                 f'/I{stub}',f'/I{inc}',f'/I{headers}',f'/Fo{obj}',source], build)
        dll = build/'long.dll'
        run([ld,'/NOLOGO','/DLL','/MACHINE:X64',f'/OUT:{dll}',*[f'/LIBPATH:{p}' for p in libs],*objects],build)
        child = subprocess.run([sys.executable,str(Path(__file__).resolve()),'--worker',str(dll)],cwd=build,
            capture_output=True,text=True,timeout=30)
        require(child.returncode==0,'Native child: '+child.stdout+child.stderr)
        result = json.loads(child.stdout)
    require(all(p.read_bytes()==v for p,v in before.items()), 'Inputs changed')
    print(json.dumps(dict(**result,hardware=False,portable=portable,source_sha256={str(p.relative_to(ROOT)):hashlib.sha256(v).hexdigest() for p,v in before.items()}),indent=2))

if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--worker': worker(Path(sys.argv[2]))
    else: require(sys.argv[1:] in ([],['--portable']),'Offline only'); main('--portable' in sys.argv)
