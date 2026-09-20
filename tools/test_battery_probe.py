"""Production battery probe, mocked registers/time only; no device access."""
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from test_pdm_frame_math import ROOT, compiler_paths, require, run

def main():
    source=ROOT/'tools/tests/battery_probe_native.c'
    paths=[source,source.with_name('battery_probe_platform.h'),ROOT/'usb_firmware/src/battery_probe.c',
           ROOT/'usb_firmware/src/battery_probe.h',ROOT/'usb_firmware/src/battery_restore.inc',
           source.with_name('battery_restore_model.inc'),source.with_name('battery_service_model.inc'),
           *[ROOT/'usb_firmware/src'/n for n in ('battery_service.inc','battery_power.c','battery_power.h')],Path(__file__).resolve()]
    before={p:p.read_bytes() for p in paths}
    cc,ld,inc=compiler_paths();sdk=Path('C:/Program Files (x86)/Windows Kits/10')
    headers=sorted((sdk/'Include').glob('*/ucrt'))[-1]
    libs=[inc.parent/'lib/x64',sdk/'Lib'/headers.parent.name/'ucrt/x64',sdk/'Lib'/headers.parent.name/'um/x64']
    with tempfile.TemporaryDirectory(prefix='pendant-battery-') as folder:
        build=Path(folder);obj=build/'probe.obj';dll=build/'battery.dll'
        run([cc,'/nologo','/c','/TC','/std:c11','/O1','/MD','/W4','/WX',f'/I{inc}',f'/I{headers}',
             f'/I{source.parent}',f'/Fo{obj}',source],build)
        run([ld,'/NOLOGO','/DLL','/MACHINE:X64',f'/OUT:{dll}',*[f'/LIBPATH:{p}' for p in libs],obj],build)
        child=subprocess.run([sys.executable,str(Path(__file__).resolve()),'--worker',str(dll)],
                             cwd=build,capture_output=True,text=True,timeout=30)
        require(child.returncode==0,'Native checks: '+child.stdout+child.stderr);result=json.loads(child.stdout)
    require(all(p.read_bytes()==v for p,v in before.items()),'Source changed')
    print(json.dumps(dict(**result,hardware=False,source_sha256={str(p.relative_to(ROOT)):hashlib.sha256(v).hexdigest() for p,v in before.items()}),indent=2))

if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--worker':
        path=Path(sys.argv[2]).resolve();require(path.name=='battery.dll' and path.parent.name.startswith('pendant-battery-'),'Unexpected DLL')
        dll=C.CDLL(str(path));out=(C.c_uint64*2)();dll.battery_tests.argtypes=[C.POINTER(C.c_uint64)]
        require(dll.battery_tests(out)==0,'Native return');print(json.dumps(dict(result='PASS',assertions=out[0],groups=out[1])))
    else:require(len(sys.argv)==1,'Offline only');main()
