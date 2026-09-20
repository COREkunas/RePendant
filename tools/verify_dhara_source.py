"""Offline portable verification of exactly the Dhara files compiled by firmware."""
import argparse
import hashlib
from pathlib import Path

PINS = {
    "dhara/map.c": "77ba1cedfc319dacafb5847f97d1467de6c0a19e622f92cb442f69c705192ed3",
    "dhara/journal.c": "ac98c9424855ea7aad318807ade621599c01c6a0d612d15f5146a81bb2f1fb21",
    "dhara/error.c": "c71726c12b9884a2716c6861b109eeaee13d3b65d28b9b29de32b01498631484",
    "include/dhara/bytes.h": "b4138fc271ed2757e62b4aa6eab7f3eb0176da5d53470e0166a8780d46cd0e88",
    "include/dhara/error.h": "87196176947857f85d184cc9b1f5dfc07e490366dc3ed06ecef9c78a397f2743",
    "include/dhara/journal.h": "443c16e33a8f7a78a84c6619c8dfc52a6c060b710d85583a64758e7e3a967c13",
    "include/dhara/map.h": "38f0d1f4e6bbccfb2d2999c0ad76c790eb55a974d5bfb3a08f5ca214c6c67cb6",
    "include/dhara/nand.h": "1d0bb8ff2d14ae7a3303bfd47e6b4e8fa338c94abc58aa1e87cbdb087ad17b16",
}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    for name, expected in PINS.items():
        if hashlib.sha256((args.source / name).read_bytes()).hexdigest() != expected:
            raise SystemExit('Pinned Dhara source differs: ' + name)
    print('Verified all 8 pinned Dhara compilation/header files; no hardware accessed.')
