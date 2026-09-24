"""Actual pure C policy, compiled locally. No device access."""
import ctypes as C
import json
from pathlib import Path
import tempfile
# Public export: use the existing standalone compiler helper without importing
# the private development runtime harness and its unrelated Dhara fixtures.
from test_pdm_frame_math import ROOT,compiler_paths,run,require

def main():
    cc,ld,inc=compiler_paths()
    sdk=Path('C:/Program Files (x86)/Windows Kits/10')
    headers=sorted((sdk/'Include').glob('*/ucrt'))[-1]
    libs=[inc.parent/'lib/x64',sdk/'Lib'/headers.parent.name/'ucrt/x64',sdk/'Lib'/headers.parent.name/'um/x64']
    with tempfile.TemporaryDirectory(prefix='pendant-preferences-') as directory:
        build=Path(directory);obj=build/'policy.obj';dll=build/'policy.dll'
        run([cc,'/nologo','/c','/TC','/std:c11','/O1','/MD','/W4','/WX',f'/I{inc}',f'/I{headers}',
             f'/Fo{obj}',ROOT/'usb_firmware/src/device_preferences.c'],build)
        fixture=build/'fixture.obj'
        run([cc,'/nologo','/c','/TC','/std:c11','/O1','/MD','/W4','/WX','/wd4505',f'/I{inc}',f'/I{headers}',
             f'/I{ROOT/"tools/tests/preferences"}',f'/I{ROOT/"usb_firmware/src"}',f'/Fo{fixture}',
             ROOT/'tools/tests/preferences_persistence_native.c'],build)
        names=['dp_defaults','dp_upgrade','dp_blink','dp_valid','dp_revision','dp_next','dp_idle_ms','dp_advertising_units','dp_color']
        run([ld,'/NOLOGO','/DLL','/MACHINE:X64',f'/OUT:{dll}',*[f'/LIBPATH:{p}' for p in libs],
             *[f'/EXPORT:{name}' for name in names],obj,fixture],build)
        lib=C.CDLL(str(dll));array=C.c_uint8*16
        lib.dp_color.argtypes=[C.POINTER(C.c_uint8),C.c_int,C.c_uint64,C.POINTER(C.c_uint8)]
        p=array();lib.dp_defaults(p)
        require(bytes(p)==bytes([2,0,32,1,4,0,0,1,25,1,0,0,0,0,0,0]),'golden default')
        require(lib.dp_valid(p,16)==1,'valid default')
        require(not lib.dp_valid(p,15) and not lib.dp_valid(None,16),'size/null')
        checks=3
        for index,values in enumerate(([1,2],range(3),range(8,65),range(1,8),range(1,8),range(8),range(8),range(1,8),range(20,51),range(4),range(2),range(8))):
            for value in range(256):
                q=array.from_buffer_copy(p);q[index]=value
                require(bool(lib.dp_valid(q,16))==(value in values),f'field {index}/{value}')
                checks+=1
        out=array();require(not lib.dp_next(p,p,out) and out[12]==1,'CAS revision increment');checks+=1
        require(lib.dp_next(out,p,p)!=0,'stale revision');checks+=1
        for profile,units in [(0,48),(1,400),(2,1600)]:
            p[1]=profile
            require(lib.dp_advertising_units(p,0)==48 and lib.dp_advertising_units(p,1)==units,'idle interval')
            checks+=1
        p[9]=0;require(lib.dp_advertising_units(p,1)==48,'never idle');checks+=1
        rgb=(C.c_uint8*3)()
        lib.dp_color(p,1,0,rgb);require(bytes(rgb)==bytes([32,0,0]),'record red')
        lib.dp_color(p,2,0,rgb);require(bytes(rgb)==bytes([32,32,0]),'low yellow')
        lib.dp_color(p,2,250,rgb);require(bytes(rgb)==bytes(3),'low blink off')
        p[3]=3;lib.dp_color(p,1,999999,rgb);require(bytes(rgb)==bytes([0,0,32]),'record blue always on');checks+=4
        persistence_checks=lib.preferences_persistence_tests()
        require(persistence_checks>=35,'persistence groups')
        # Explicitly release native library before TemporaryDirectory cleanup on Windows.
        import _ctypes
        _ctypes.FreeLibrary(lib._handle)
    print(json.dumps(dict(result='PASS',checks=checks,persistence_checks=persistence_checks,hardware=False)))
if __name__=='__main__':main()
