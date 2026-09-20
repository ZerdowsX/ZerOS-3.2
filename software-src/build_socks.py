#!/usr/bin/env python3
"""
build_socks.py - compiles a single-file .socks program from C source.

Usage: build_socks.py <source.c> <program_name> <output.socks>

This uses the exact same gcc used to build the kernel itself (freestanding,
no libc), just linked at a different fixed address (SOCKS_LOAD_ADDR) via a
generated linker script, so the resulting flat binary can be dropped
straight into memory and run by the kernel's .socks loader.
"""
import subprocess, sys, struct, os, re

SOCKS_LOAD_ADDR = 0x700000
HEADER_SIZE = 4 + 4 + 4*4 + 4 + 32  # magic + version + 4 entry offsets + code_size + name
SOCKS_MAGIC = b"SOCK"
SOCKS_VERSION = 1

def run(cmd):
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    src, name, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    workdir = os.path.dirname(os.path.abspath(out_path)) or "."
    obj = os.path.join(workdir, "_socks_tmp.o")
    elf = os.path.join(workdir, "_socks_tmp.elf")
    ld_script = os.path.join(workdir, "_socks_tmp.ld")

    code_addr = SOCKS_LOAD_ADDR + HEADER_SIZE
    with open(ld_script, "w") as f:
        f.write(f"""
ENTRY(init)
SECTIONS {{
    . = {hex(code_addr)};
    .text : {{ *(.text*) }}
    .rodata : {{ *(.rodata*) }}
    .data : {{ *(.data*) }}
    .bss : {{ *(.bss*) *(COMMON) }}
}}
""")

    cflags = ["-m64", "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
              "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2", "-O1", "-c"]
    run(["gcc"] + cflags + [src, "-o", obj])
    run(["ld", "-n", "-T", ld_script, "-nostdlib", obj, "-o", elf])

    # Find symbol addresses for our known entry points via nm
    nm_out = subprocess.run(["nm", elf], capture_output=True, text=True, check=True).stdout
    symbols = {}
    for line in nm_out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            addr, kind, sym = parts
            symbols[sym] = int(addr, 16)

    def offset_for(fn_name):
        if fn_name not in symbols:
            return 0
        return symbols[fn_name] - SOCKS_LOAD_ADDR

    entry_init = offset_for("init")
    entry_draw = offset_for("draw")
    entry_key = offset_for("handle_key")
    entry_click = offset_for("handle_click")

    # Extract the flat binary (just .text+.rodata+.data+.bss as laid out by the linker script)
    raw_bin = os.path.join(workdir, "_socks_tmp.bin")
    run(["objcopy", "-O", "binary", elf, raw_bin])
    with open(raw_bin, "rb") as f:
        code = f.read()

    name_bytes = name.encode()[:31]
    name_bytes = name_bytes + b"\x00" * (32 - len(name_bytes))

    header = SOCKS_MAGIC
    header += struct.pack("<I", SOCKS_VERSION)
    header += struct.pack("<I", entry_init)
    header += struct.pack("<I", entry_draw)
    header += struct.pack("<I", entry_key)
    header += struct.pack("<I", entry_click)
    header += struct.pack("<I", len(code))
    header += name_bytes

    assert len(header) == HEADER_SIZE, f"header size mismatch: {len(header)} != {HEADER_SIZE}"

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(code)

    for tmp in (obj, elf, ld_script, raw_bin):
        if os.path.exists(tmp):
            os.remove(tmp)

    print(f"\nBuilt {out_path}: {len(header)+len(code)} bytes "
          f"(init={entry_init:#x} draw={entry_draw:#x} key={entry_key:#x} click={entry_click:#x})")

if __name__ == "__main__":
    main()
