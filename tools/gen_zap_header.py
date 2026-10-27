import os, struct

ROOT = r"C:\hjkkik\ios7pro"
ZDIR = os.path.join(ROOT, r"work\firmware\zap")
OUT = os.path.join(ROOT, r"work\gpu\zap_image.h")

parts = ["a640_zap.mdt", "a640_zap.b00", "a640_zap.b01", "a640_zap.b02"]
blob = b""
offsets = {}
for p in parts:
    data = open(os.path.join(ZDIR, p), 'rb').read()
    offsets[p] = (len(blob), len(data))
    blob += data

print("combined %d bytes" % len(blob), offsets)

lines = []
lines.append("/*")
lines.append(" * a640 zap shader image, assembled the way qcom_mdt does it: the .mdt ELF")
lines.append(" * header is followed by its segments in order (b00 hash, b01 program, b02")
lines.append(" * signature), so every p_offset in the ELF indexes this one buffer.")
lines.append(" * Extracted from the stock vendor image (work/vendor.img, /firmware/).")
lines.append(" * Generated - do not edit.")
lines.append(" */")
lines.append("#ifndef _SWEET_GPU_ZAP_H_")
lines.append("#define _SWEET_GPU_ZAP_H_")
lines.append("")
lines.append("#define SWEET_ZAP_SIZE\t%du" % len(blob))
lines.append("")
lines.append("static const uint8_t sweet_zap_image[SWEET_ZAP_SIZE] = {")
row = []
for i, b in enumerate(blob):
    row.append("0x%02x" % b)
    if len(row) == 16:
        lines.append("\t" + ",".join(row) + ",")
        row = []
if row:
    lines.append("\t" + ",".join(row) + ",")
lines.append("};")
lines.append("")
lines.append("#endif /* _SWEET_GPU_ZAP_H_ */")
open(OUT, 'w', encoding='utf-8', newline='\n').write("\n".join(lines) + "\n")
print("wrote", OUT, len(lines), "lines")
