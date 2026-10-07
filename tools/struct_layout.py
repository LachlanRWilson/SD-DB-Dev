#!/usr/bin/env python3
"""
Print struct/union layouts from source/app/include, then the PASS/FAIL result
of every STATIC_ASSERT with the values of the defines (mem_layout.h and others)
each assert uses. Both sections are grouped by header.

_Static_assert is compiled out so the layout still prints while asserts fail.

Usage: python3 tools/struct_layout.py [--m32] [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "source" / "app" / "include"
MEM_LAYOUT = "mem_layout.h"
DEFS = ["-DHOST_BUILD=1", "-DUSE_SW_CRC=1", "-D_Static_assert(a,b)="]

IDENT = re.compile(r"\b[A-Za-z_]\w*\b")
STRUCT_RE = re.compile(
    r"typedef\s+(struct|union)\s*\w*\s*\{(?P<body>[^{}]*)\}\s*(?P<name>\w+)\s*;")
DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)(?!\()[ \t]*(.*)$", re.M)


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def join_continuations(src):
    return re.sub(r"\\\n", " ", src)


def header_compiles(cc, flags, header):
    r = subprocess.run([cc, "-x", "c", "-std=c11", "-fsyntax-only", *flags,
                        f"-I{INCLUDE}", "-"],
                       input=f'#include "{header.name}"\n',
                       capture_output=True, text=True)
    return r.returncode == 0


def parse_structs(src):
    structs = []
    for m in STRUCT_RE.finditer(src):
        members = []
        for decl in m.group("body").split(";"):
            fn_ptr = re.search(r"\(\s*\*\s*(\w+)\s*\)", decl)
            decl = re.sub(r"\[.*?\]", "", decl).strip()
            if fn_ptr:
                members.append(fn_ptr.group(1))
            elif decl:
                members.append(IDENT.findall(decl)[-1])
        structs.append((m.group("name"), members))
    return structs


def preprocessed_by_header(cc, flags, headers):
    """Preprocess all headers together and split the output back per header,
    so struct bodies only contain the #if branches that are actually active."""
    src = "".join(f'#include "{h.name}"\n' for h in headers)
    r = subprocess.run([cc, "-x", "c", "-std=c11", "-E", *flags, f"-I{INCLUDE}", "-"],
                       input=src, capture_output=True, text=True, check=True)
    chunks, cur = {}, None
    for line in r.stdout.splitlines():
        marker = re.match(r'#\s*\d+\s+"([^"]+)"', line)
        if marker:
            path = Path(marker.group(1))
            cur = path.name if path.parent.resolve() == INCLUDE.resolve() else None
        elif cur:
            chunks.setdefault(cur, []).append(line)
    return {name: "\n".join(lines) for name, lines in chunks.items()}


def parse_asserts(src):
    """Return the condition (first argument) of every STATIC_ASSERT call."""
    conds = []
    for m in re.finditer(r"\bSTATIC_ASSERT\s*\(", src):
        depth, i, start = 1, m.end(), m.end()
        comma = None
        while depth and i < len(src):
            c = src[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == "," and depth == 1 and comma is None:
                comma = i
            elif c == '"':
                i = src.index('"', i + 1)
            i += 1
        if comma is not None:
            conds.append(" ".join(src[start:comma].split()))
    return conds


def parse_defines(src):
    return {name: body.strip() for name, body in DEFINE_RE.findall(src) if body.strip()}


def c_str(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", default="gcc", help="host C compiler (default gcc)")
    ap.add_argument("--m32", action="store_true", help="compile with -m32 (closer to STM32 layout)")
    args = ap.parse_args()
    flags = DEFS + (["-m32"] if args.m32 else [])

    headers, skipped = [], []
    for h in sorted(INCLUDE.glob("*.h")):
        (headers if header_compiles(args.cc, flags, h) else skipped).append(h)

    sources = {h.name: join_continuations(strip_comments(h.read_text())) for h in headers}
    defines = {}
    for name, src in sources.items():
        for macro, body in parse_defines(src).items():
            defines.setdefault(macro, (name, body))

    expanded = preprocessed_by_header(args.cc, flags, headers)
    structs = {h.name: parse_structs(expanded.get(h.name, "")) for h in headers}
    asserts = [(name, cond) for name, src in sources.items() for cond in parse_asserts(src)]

    def deps(cond):
        """Defines referenced by an assert, plus the defines those depend on."""
        found, stack = [], IDENT.findall(cond)
        while stack:
            ident = stack.pop(0)
            if ident in defines and ident not in found and ident != "STATIC_ASSERT":
                found.append(ident)
                stack.extend(IDENT.findall(defines[ident][1]))
        return found

    def emit_check(cond):
        """Print an assert's PASS/FAIL, then the defines it uses."""
        out.append(f'if ({cond}) printf("PASS  %s\\n", "{c_str(cond)}"); '
                   f'else {{ fails++; printf("FAIL  %s\\n", "{c_str(cond)}"); }}')
        for m in deps(cond):
            src = defines[m][0]
            label = m if src == MEM_LAYOUT else f"{m} ({src})"
            out.append(f'printf("      %-38s %6lld\\n", "{label}", (long long)({m}));')

    out = ["#include <stdio.h>", "#include <stddef.h>"]
    out += [f'#include "{h.name}"' for h in headers]
    out.append("int main(void) {")
    out.append("int fails = 0;")
    out.append('printf("===== Structs =====\\n");')
    for hname, slist in structs.items():
        if not slist:
            continue
        out.append(f'printf("\\n=== {hname} ===\\n");')
        for sname, members in slist:
            out.append(f'printf("%-32s %5zu\\n", "{sname}", sizeof({sname}));')
            for m in members:
                out.append(f'printf("  %-28s off %4zu  size %4zu\\n", "{m}", '
                           f'offsetof({sname}, {m}), sizeof((({sname} *)0)->{m}));')

    out.append('printf("\\n===== Static Asserts =====\\n");')
    for hname in structs:
        hasserts = [cond for name, cond in asserts if name == hname]
        if not hasserts:
            continue
        out.append(f'printf("\\n=== {hname} ===\\n");')
        for cond in hasserts:
            emit_check(cond)

    out.append(f'printf("\\n%d/{len(asserts)} static asserts failed\\n", fails);')
    out.append("return fails != 0;\n}")

    with tempfile.TemporaryDirectory() as tmp:
        c_file, exe = Path(tmp) / "layout.c", Path(tmp) / "layout"
        c_file.write_text("\n".join(out) + "\n")
        r = subprocess.run([args.cc, "-std=c11", "-w", *flags, f"-I{INCLUDE}",
                            str(c_file), "-o", str(exe)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"compile failed:\n{r.stderr}")
        if skipped:
            print("skipped (don't compile on host): " + ", ".join(h.name for h in skipped), flush=True)
        sys.exit(subprocess.run([str(exe)]).returncode)


if __name__ == "__main__":
    main()
