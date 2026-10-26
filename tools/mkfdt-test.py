#!/usr/bin/env python3
"""Build a test device tree for shim/probe.c and the host console renderer.

The shim's bring-up report (shim/probe.c) reads the board's live FDT; with
no phone in reach it can still be exercised in the host renderer:

    python3 tools/mkfdt-test.py /tmp/guac.dtb                # guacamole-like
    python3 tools/mkfdt-test.py /tmp/sweet.dtb --sweet       # sweet-like
    (cd shim && BOARD=guacamole DTB=/tmp/guac.dtb ./build.sh)
    ./shim/out/host_test /tmp/guac.dtb                       # -> out/probe.ppm

The tree carries only the nodes the report looks at (memory, reserved
memory, cpus, chosen and a handful of MMIO nodes) in the downstream
Qualcomm layout: big-endian tokens, 2-cell addresses per the root
#address-cells/#size-cells.
"""
import struct
import sys

FDT_MAGIC = 0xd00dfeed
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_END = 9


def s(text):
    return text.encode() + b'\0'


def pad4(b):
    return b + b'\0' * (-len(b) & 3)


def u32(*v):
    return struct.pack('>%dI' % len(v), *v)


class Node:
    def __init__(self, name, props=(), children=()):
        self.name = name
        self.props = list(props)
        self.children = list(children)


def build(root):
    names = []

    def collect(n):
        for name, _ in n.props:
            if name not in names:
                names.append(name)
        for c in n.children:
            collect(c)
    collect(root)
    offs, pos = {}, 0
    for name in names:
        offs[name] = pos
        pos += len(name) + 1
    strings_block = b''.join(n.encode() + b'\0' for n in names)

    block = []

    def emit(n):
        block.append(u32(FDT_BEGIN_NODE))
        block.append(pad4(s(n.name)))
        for name, val in n.props:
            block.append(u32(FDT_PROP, len(val), offs[name]))
            block.append(pad4(val))
        for c in n.children:
            emit(c)
        block.append(u32(FDT_END_NODE))

    emit(root)
    block.append(u32(FDT_END))
    struct_block = b''.join(block)
    off_struct = 40
    off_strings = off_struct + len(struct_block)
    total = off_strings + len(strings_block)
    header = struct.pack('>10I', FDT_MAGIC, total, off_struct, off_strings,
                         0, 17, 16, 0, 0, 0)
    return header + struct_block + strings_block


def guacamole():
    mem = Node('memory', [
        ('device_type', s('memory')),
        # 2 GiB low bank + 6 GiB high bank (8 GB SKU)
        ('reg', u32(0, 0x80000000, 0, 0x80000000, 1, 0, 1, 0x80000000)),
    ])
    rsvd = Node('reserved-memory', [('#address-cells', u32(2)), ('#size-cells', u32(2))], [
        Node('hyp_mem', [('reg', u32(0, 0x85700000, 0, 0x600000))]),
        Node('smem', [('reg', u32(0, 0x86000000, 0, 0x200000))]),
        Node('removed_regions', [('reg', u32(0, 0x86200000, 0, 0x5500000))]),
        Node('cont_splash_region', [('reg', u32(0, 0x9c000000, 0, 0x2400000))]),
        Node('disp_rdump_region', [('reg', u32(0, 0x9c000000, 0, 0x2400000))]),
        Node('qseecom_region', [('reg', u32(0, 0x9e400000, 0, 0x1400000))]),
        Node('pil_modem_region', [('reg', u32(0, 0x8d800000, 0, 0x9600000))]),
    ])
    cpus = Node('cpus', [('#address-cells', u32(1)), ('#size-cells', u32(0))], [
        Node('cpu@%x' % (i * 0x100), [('device_type', s('cpu')), ('reg', u32(0, i * 0x100))])
        for i in range(8)
    ])
    chosen = Node('chosen', [
        ('linux,initrd-start', u32(0x9f800000)),
        ('linux,initrd-end', u32(0xa0000000)),
    ])
    soc = Node('soc', [('#address-cells', u32(2)), ('#size-cells', u32(2))], [
        Node('interrupt-controller@17a00000', [
            ('compatible', s('arm,gic-v3')),
            ('reg', u32(0, 0x17a00000, 0, 0x10000, 0, 0x17a60000, 0, 0x100000)),
        ]),
        Node('timer', [('compatible', s('arm,armv8-timer')), ('interrupts', u32(1, 3, 0, 0xf08))]),
        Node('qcom,spmi@c440000', [('compatible', s('qcom,spmi-pmic-arb')), ('reg', u32(0, 0xc440000, 0, 0x1100, 0, 0xc600000, 0, 0x2000000))]),
        Node('ufshc@1d84000', [('compatible', s('qcom,ufshc')), ('reg', u32(0, 0x1d84000, 0, 0x2500))]),
        Node('ssusb@a600000', [('compatible', s('qcom,dwc-usb3-msm')), ('reg', u32(0, 0xa600000, 0, 0x100000))]),
        Node('dwc3@a600000', [('compatible', s('snps,dwc3')), ('reg', u32(0, 0xa600000, 0, 0xcd00))]),
        Node('hsphy@88e2000', [('compatible', s('qcom,usb-hsphy-snps-femto')), ('reg', u32(0, 0x88e2000, 0, 0x110))]),
        Node('ssphy@88e8000', [('compatible', s('qcom,usb-ssphy-qmp-dp-combo')), ('reg', u32(0, 0x88e8000, 0, 0x3000))]),
        Node('eud@88e0000', [('compatible', s('qcom,msm-eud')), ('reg', u32(0, 0x88e0000, 0, 0x2000))]),
        Node('pinctrl@3100000', [('compatible', s('qcom,sm8150-tlmm')), ('reg', u32(0, 0x3100000, 0, 0x300000))]),
        Node('clock-controller@100000', [('compatible', s('qcom,gcc-sm8150')), ('reg', u32(0, 0x100000, 0, 0x1f0000))]),
        Node('mailbox@18220000', [('compatible', s('qcom,tcs-drv')), ('reg', u32(0, 0x18220000, 0, 0x100, 0, 0x18220d00, 0, 0x3000))]),
        Node('qcom,msm-imem@146bf000', [('compatible', s('qcom,msm-imem')), ('reg', u32(0, 0x146bf000, 0, 0x1000))]),
        Node('restart@c264000', [('compatible', s('qcom,pshold')), ('reg', u32(0, 0xc264000, 0, 0x4))]),
        Node('noise@1', [('reg', u32(0, 0x1, 0, 0x1000))]),
    ])
    return Node('', [
        ('model', s('Qualcomm Technologies, Inc. SM8150P v2.1 MTP')),
        ('compatible', s('qcom,sm8150-mtp') + s('qcom,sm8150') + s('qcom,mtp')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [mem, rsvd, cpus, chosen, soc])


def sweet():
    mem = Node('memory', [('device_type', s('memory')),
                          ('reg', u32(0, 0x80000000, 0, 0x80000000, 1, 0, 0, 0xfcc00000))])
    return Node('', [
        ('model', s('Qualcomm Technologies, Inc. SM6150 PM6150 QRD')),
        ('#address-cells', u32(2)), ('#size-cells', u32(2)),
    ], [mem])


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        raise SystemExit(__doc__)
    blob = build(sweet() if '--sweet' in sys.argv else guacamole())
    open(args[0], 'wb').write(blob)
    print(f'{args[0]}: {len(blob)} bytes')
