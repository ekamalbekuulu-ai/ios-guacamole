"""Pack a bootable fastboot image: shim header + kernel + SWEETXNU payload.

The payload carries the ADT, the GPU firmware and the ustar tree that
bsd/dev/sweet_overlay.c installs into the root volume at boot, so adding the
Metal bundle costs an 18 MB RAM boot instead of an 11 GB userdata flash.

usage: pack_mtl.py <kernel> <out.img> [halt]
"""
import os
import struct
import sys

ROOT = r"C:\hjkkik\ios7pro"
REPO = os.path.join(ROOT, "our-repo-x", "ios-guacamole-main")
BASE = os.path.join(ROOT, "work", "a6wd", "a6wd6", "shim-boot-a6wd6.img")
FW = os.path.join(REPO, "firmware")
OVL = os.path.join(ROOT, "work", "ovlroot")

kern = sys.argv[1]
out = sys.argv[2]
halt = len(sys.argv) > 3 and sys.argv[3] == "halt"

args = [
    "debug=0xc", "serial=3", "cpus=8", "keepsyms=1", "-noprogress",
    "rd=ufsroot0", "unentitled_ios_sim_launch=1", "ignition_level=1",
    "wdt_test=no_user_checkin", "ufsrw=1", "rootrw=1",
    "launchd_unsecure_cache=1", "wdt=-1",
    "sweetusb=0", "sweettouchdelay=20", "sweetfb_hz=30",
    "sweetlogdump=1", "-enable_kprintf_spam",
    # sweetmtl stays 0 on purpose: SweetMetal.bundle ships without its
    # pipelines/*.json templates (the turnip capture they come from was never
    # published), and a published Metal device that cannot build a pass draws
    # nothing, which costs the UI its content. The bundle is installed now so
    # that enabling it later is a boot-arg, not a flash.
    "sweeta6xx=1", "sweetzap=0x98b15000", "sweetmtl=0",
    "sweetfb_fill=1", "sweetbl=85", "sweetbattery=1", "sweetbatt=1",
]
if halt:
    args.append("sweetovl_halt=1")
cmd = " ".join(args)

b = bytearray(open(BASE, "rb").read())
ks, ka, rs, ra, ss, sa, ta, ps = struct.unpack_from("<8I", b, 0x08)
pg = ps
off = lambda n: ((n + pg - 1) // pg) * pg
ko = pg
ro = ko + off(ks)
so = ro + off(rs)
rest = bytes(b[so + off(ss):])
kernel = bytes(b[ko:ko + ks])
old = bytes(b[ro:ro + rs])
second = bytes(b[so:so + ss])

v, e, ao, asz = struct.unpack_from("<4Q", old, 0x10)
adt = os.path.join(ROOT, "work", "kovd3", "guac.adt")
open(adt, "wb").write(old[ao:ao + asz])

sys.path.insert(0, os.path.join(REPO, "tools"))
import mkpayload

newp, info = mkpayload.build(kern, adt, cmd, None, FW, OVL)
hdr = bytearray(b[:pg])
struct.pack_into("<I", hdr, 0x10, len(newp))
body = (bytes(hdr) + kernel + b"\x00" * (-len(kernel) % pg) + newp
        + b"\x00" * (-len(newp) % pg) + second + b"\x00" * (-len(second) % pg) + rest)
open(out, "wb").write(body)

# read the cmdline back out of the payload we just wrote, as the device will
pro = pg + off(ks)
magic = body[pro:pro + 8]
got = body[pro + 0x48:pro + 0x48 + 600].split(b"\x00")[0].decode()
print("wrote %s  %.2f MB  payload_magic=%s  kernel=%d" %
      (out, len(body) / 2 ** 20, magic.decode("ascii", "replace"), len(kernel)))
print("halt =", halt)
print("cmdline =", got)
assert magic == b"SWEETXNU", "payload magic wrong"
