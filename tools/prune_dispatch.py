#!/usr/bin/env python3
"""
prune_dispatch.py - drop unreached functions from the generated dispatch table.

The recompiler's function finder deliberately accepts false positives: on a PC
the dead code costs nothing, but on a 4MB DS it is the difference between
fitting and not. The build already uses -ffunction-sections -Wl,--gc-sections,
so the linker WOULD drop unreferenced functions - except the dispatch table
references every one of them, which is what keeps the whole 2.6MB alive.

So the prune happens here: keep only the cases whose function appears in the
coverage whitelist, and let --gc-sections do the rest.

Removed cases are NOT silently dropped. They fall through to a stub that
reports the address, because the whitelist is a lower bound: it reflects what
the coverage scripts happened to execute, and a path they never took would
otherwise become undefined behaviour rather than a visible error.

Usage:
    python3 tools/prune_dispatch.py coverage.txt \\
        generated/super-mario-bros_dispatch.c \\
        generated/super-mario-bros_dispatch_pruned.c
"""
import re
import sys


CASE_RE = re.compile(r"^\s*case\s+(0x[0-9A-Fa-f]+):\s*$")
CALL_RE = re.compile(r"^\s*(func_[0-9A-Za-z_]+)\s*\(\s*\)\s*;\s*break\s*;\s*$")


def load_whitelist(path):
    names = set()
    with open(path, "r", errors="replace") as f:
        for line in f:
            name = line.strip()
            if name.startswith("func_"):
                names.add(name)
    return names


def prune(whitelist, src_path, out_path):
    with open(src_path, "r", newline="", errors="replace") as f:
        lines = f.readlines()

    out = []
    kept = dropped = 0
    i = 0
    while i < len(lines):
        m = CASE_RE.match(lines[i].rstrip("\r\n"))
        if m and i + 1 < len(lines):
            call = CALL_RE.match(lines[i + 1].rstrip("\r\n"))
            if call:
                if call.group(1) in whitelist:
                    out.append(lines[i])
                    out.append(lines[i + 1])
                    kept += 1
                else:
                    dropped += 1
                i += 2
                continue
        out.append(lines[i])
        i += 1

    # Route everything pruned to a reporting stub rather than silence.
    #
    # The outer switch's default already falls to nes_interp_dispatch(), which
    # is a no-op stub in the DS build - so without this change every pruned
    # address would silently do nothing, which is the worst possible outcome.
    # The outer default is the two-line form; the inner ones (single-line) are
    # real interpreter fallbacks and must be left alone.
    text = "".join(out)
    if "nes_dispatch_pruned" not in text:
        text = text.replace(
            "int call_by_address_cb(uint16_t addr, int _caller_bank) {",
            "extern int nes_dispatch_pruned(uint16_t addr);\n\n"
            "int call_by_address_cb(uint16_t addr, int _caller_bank) {",
            1,
        )
    for nl in ("\r\n", "\n"):
        outer = f"        default:{nl}            return nes_interp_dispatch(addr);"
        if outer in text:
            text = text.replace(
                outer,
                f"        default:{nl}            return nes_dispatch_pruned(addr);",
            )
            break

    with open(out_path, "w", newline="") as f:
        f.write(text)

    return kept, dropped


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1

    whitelist = load_whitelist(sys.argv[1])
    if not whitelist:
        print("whitelist is empty - is coverage.txt the right file?")
        return 1

    kept, dropped = prune(whitelist, sys.argv[2], sys.argv[3])
    total = kept + dropped
    if total == 0:
        print("no dispatch cases matched - has the generated format changed?")
        return 1

    print(f"whitelist   : {len(whitelist)} functions")
    print(f"cases kept  : {kept}")
    print(f"cases pruned: {dropped}  ({dropped * 100 // total}%)")
    print(f"written     : {sys.argv[3]}")
    print()
    print("Now build against the pruned file and compare .text. Anything the")
    print("coverage run never reached will call nes_dispatch_pruned() at")
    print("runtime and print the address, rather than failing silently.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
