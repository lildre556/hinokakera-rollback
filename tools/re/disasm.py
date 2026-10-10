"""Disassemble hinokakera.exe at an address: disasm.py 0x401000 [count]

The CE build by default; HINO_EXE picks another exe (E:/Hinokakera/vanilla/hinokakera.exe is ver.3.00).
"""
import os
import sys

import capstone
import pefile

EXE = os.environ.get("HINO_EXE", r"E:\Hinokakera\ce_vanilla\hinokakera.exe")
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
addr = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
for ins in md.disasm(img[addr - base: addr - base + count * 15], addr, count):
    print(f"{ins.address:#x}: {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
