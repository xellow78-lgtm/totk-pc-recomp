"""The hook lines of a mod's plugin (recomp/patches.py `hook`, `inline`, `installer`), recorded by running the plugin's
start-up code under Unicorn against the game's main.

An exlaunch plugin (a mod's exefs/subsdk1..9) installs its hooks while it starts: for each one it calls its hook
installer with the game function and its own function, and the installer writes a branch into the game's code. The
port builds those hooks in instead (runtime/plugin.c), and for that the generator must know every function and
instruction the plugin hooks. This tool finds them: it loads main and the plugin at the bases recomp/layout.py gives
them, answers the plugin's SVCs the way runtime/plugin.c does (the same view of memory: plugin.c query), runs the
plugin's DT_INIT, and at each call of an installer notes the target and returns instead of letting it run, as the
runtime's own installers do. The game's code is left as shipped, which the plugin checks its targets against.

  python tools/plugin_hooks.py --exefs <dir with main, subsdk0, sdk and the plugin> --plugin subsdk7
                               --hook-installer 0x33350 --inline-installer 0x3a020 [--out FILE] [-v]

The two installers are functions of the plugin, found by reading it: the hook installer takes (function, replacement,
wants a trampoline) and returns the trampoline; the inline installer takes (instruction, function, with vector
registers), calls the hook installer and builds a stub that saves X0-X29 around the function. Their offsets belong to
one build of one plugin. -v prints every SVC, hook and line the plugin writes to the debug output.

The output goes after the game's patch file (cat recomp/patches-v143.txt <out>) for gen.py --patches. The words on
the lines are the first instruction at each address, which gen.py checks against the modules. It runs no game code:
the count of blocks executed inside main is printed and expected to be 0.

Needs unicorn (tools/requirements.txt has it for the differential tests). Nothing it reads is written anywhere but
the hook lines.
"""
import argparse
import ctypes
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))

from layout import GAME, layout  # noqa: E402
from nso import Nso  # noqa: E402

PAGE = 0x1000
# As runtime/plugin.c: the dynamic linker's stand-in below main, the room for the plugin's second views of code
STANDIN = 3 * PAGE
ALIAS_BASE, ALIAS_SIZE = 0x7100000000, 0x40000000
# Emulator-only places, outside everything the plugin is told about
STACK, STACK_SIZE, TLS, STUBS, STOP, TRAMP, LIST = 0x110000000, 0x100000, 0x120000000, 0x130000000, 0x131000000, 0x132000000, 0x133000000
R_RELATIVE, R_GLOB_DAT, R_JUMP_SLOT, R_ABS64 = 1027, 1025, 1026, 257


def up(v):
    return (v + PAGE - 1) & ~(PAGE - 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--plugin', required=True, help='the plugin module in --exefs, e.g. subsdk7')
    ap.add_argument('--hook-installer', type=lambda v: int(v, 16), required=True, help='plugin offset, hex')
    ap.add_argument('--inline-installer', type=lambda v: int(v, 16), required=True, help='plugin offset, hex')
    ap.add_argument('--out', type=Path)
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()
    from unicorn import UC_ARCH_ARM64, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_MODE_ARM, \
        UC_PROT_ALL, Uc, UcError
    from unicorn import arm64_const as A

    mods = {n: (b, Nso(str(args.exefs / n))) for n, b, _ in layout(args.exefs)}
    if args.plugin not in mods or args.plugin in GAME:
        sys.exit(f'{args.plugin} is no plugin module of {args.exefs}')
    main_base, mn = mods['main']
    plug_base, plug = mods[args.plugin]
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    keep = []   # the buffers Unicorn maps by pointer

    def map_module(base, nso):
        size = up(nso.size) + PAGE
        buf = (ctypes.c_ubyte * size)()
        ctypes.memmove(buf, bytes(nso.image), len(nso.image))
        keep.append(buf)
        uc.mem_map_ptr(base, size, UC_PROT_ALL, ctypes.addressof(buf))
        return buf

    bufs = {n: map_module(b, nso) for n, (b, nso) in mods.items()}
    before = bytes(bufs['main'])
    for a, n in ((main_base - STANDIN, STANDIN), (STACK, STACK_SIZE), (TLS, PAGE), (STUBS, PAGE), (STOP, PAGE),
                 (TRAMP, 0x10000), (LIST, PAGE)):
        uc.mem_map(a, n)
    uc.mem_write(LIST, struct.pack('<QQ', LIST, LIST))   # g_pAutoLoadList: an empty list, as plugin.c gives

    # the plugin linked as the loader links it: its own symbols, then stubs for what it imports
    imports = []

    def stub(name):
        if name not in imports:
            imports.append(name)
        return STUBS + 16 * imports.index(name)

    for off, typ, sym, add in plug.relocs:
        if typ == R_RELATIVE:
            v = plug_base + add
        else:
            name, _, shndx, value, _ = plug.syms[sym]
            v = plug_base + value + (add if typ == R_ABS64 else 0) if shndx else \
                LIST if name == '_ZN2nn2ro6detail15g_pAutoLoadListE' else stub(name)
        uc.mem_write(plug_base + off, struct.pack('<Q', v))
    for i in range(256):
        uc.mem_write(STUBS + 16 * i, struct.pack('<II', 0xD4200000 | i << 5, 0xD65F03C0))   # brk #i; ret
    uc.mem_write(STOP, struct.pack('<I', 0xD4200000 | 0xFFFF << 5))

    def where(a):
        for n, (b, nso) in mods.items():
            if b <= a < b + up(nso.size):
                return f'{n}+{a - b:#x}'
        return f'{a:#x}'

    hooks, inlines, maps, log = [], [], [], []

    def query(a):
        """runtime/plugin.c query(): (base, size, state, perm)."""
        def module(base, text, data_off, size):
            if not base <= a < base + size:
                return None
            off = a - base
            return (base, text, 3, 5) if off < text else (base + text, data_off - text, 3, 1) if off < data_off \
                else (base + data_off, size - data_off, 4, 3)
        r = module(main_base - STANDIN, PAGE, 2 * PAGE, STANDIN)
        for b, nso in mods.values():
            r = r or module(b, up(nso.text[0] + nso.text[1]), nso.data[0], up(nso.size))
        if r:
            return r
        if ALIAS_BASE <= a < ALIAS_BASE + ALIAS_SIZE:
            lo, hi = ALIAS_BASE, ALIAS_BASE + ALIAS_SIZE
            for dst, size in maps:
                if dst <= a < dst + size:
                    return (dst, size, 5, 3)
                lo, hi = (max(lo, dst + size), hi) if dst + size <= a else (lo, min(hi, dst))
            return (lo, hi - lo, 0, 0)
        # the rest, as the game is told with host memory inaccessible: free below 4 GiB here, nothing else mapped
        edges = sorted({0, 1 << 32, main_base - STANDIN, ALIAS_BASE, ALIAS_BASE + ALIAS_SIZE, 1 << 39}
                       | {e for b, nso in mods.values() for e in (b, b + up(nso.size))}) + [0]
        lo = hi = 0
        for i in range(len(edges) - 1):
            if edges[i] <= a and (a < edges[i + 1] or not edges[i + 1]):
                lo, hi = edges[i], edges[i + 1]
        return (lo, (hi - lo) & (1 << 64) - 1, 0x10 if lo else 0, 0)

    def svc(n):
        x = [uc.reg_read(A.UC_ARM64_REG_X0 + i) for i in range(4)]
        ret = 0
        if n == 0x06:
            base, size, state, perm = query(x[2])
            uc.mem_write(x[0], struct.pack('<QQIIIIII', base, size, state, 0, perm, 0, 0, 0))
            uc.reg_write(A.UC_ARM64_REG_X1, 0)
        elif n == 0x29:
            v = {12: ALIAS_BASE, 13: ALIAS_SIZE, 65001: 0xFFFF8001, 2: 0x1000000000, 3: 0x1000000000, 4: 0x180000000,
                 5: 0x100000000, 14: STACK, 15: STACK_SIZE}.get(x[1] & 0xFFFFFFFF)
            if v is None:
                raise RuntimeError(f'svc GetInfo id {x[1] & 0xFFFFFFFF} at {where(uc.reg_read(A.UC_ARM64_REG_PC))}')
            uc.reg_write(A.UC_ARM64_REG_X1, v)
        elif n == 0x74:   # MapProcessMemory: a copy, as in the runtime
            uc.mem_map(x[0], x[3])
            uc.mem_write(x[0], bytes(uc.mem_read(x[2], x[3])))
            maps.append((x[0], x[3]))
        elif n == 0x75:
            uc.mem_unmap(x[0], x[3])
            maps[:] = [m for m in maps if m[0] != x[0]]
        elif n == 0x27:
            log.append(bytes(uc.mem_read(x[0], min(x[1], 1024))).decode('utf-8', 'replace').rstrip())
            if args.verbose:
                print('  plugin:', log[-1])
        elif n == 0x1E:
            ret = 123456789
        else:
            raise RuntimeError(f'svc {n:#x} at {where(uc.reg_read(A.UC_ARM64_REG_PC))}: not one runtime/plugin.c answers')
        uc.reg_write(A.UC_ARM64_REG_X0, ret)

    def on_intr(uc, intno, user):
        pc = uc.reg_read(A.UC_ARM64_REG_PC)
        for at in (pc - 4, pc):   # Unicorn reports an exception at or after its instruction, by version
            try:
                w = struct.unpack('<I', bytes(uc.mem_read(at, 4)))[0]
            except UcError:
                continue
            if w & 0xFFE0001F == 0xD4000001:
                svc(w >> 5 & 0xFFFF)
                if at == pc:
                    uc.reg_write(A.UC_ARM64_REG_PC, pc + 4)
                return
            if w & 0xFFE0001F == 0xD4200000:
                if w >> 5 & 0xFFFF == 0xFFFF:
                    uc.emu_stop()
                    return
                raise RuntimeError(f'the plugin calls its import {imports[w >> 5 & 0xFFFF]} while it starts')
        raise RuntimeError(f'exception {intno} at {where(pc)}')

    def on_installer(uc, addr, size, user):
        x = [uc.reg_read(A.UC_ARM64_REG_X0 + i) for i in range(3)]
        lr = uc.reg_read(A.UC_ARM64_REG_LR)
        if addr == plug_base + args.hook_installer:
            hooks.append((x[0], x[1]))
            uc.reg_write(A.UC_ARM64_REG_X0, TRAMP + 16 * len(hooks) if x[2] & 1 else 0)
            what = 'hook'
        else:
            inlines.append((x[0], x[1], x[2] & 1))
            what = 'inline hook'
        uc.reg_write(A.UC_ARM64_REG_PC, lr)
        if args.verbose:
            print(f'  {what} {where(x[0])} -> {where(x[1])} (from {where(lr - 4)})')

    in_main = [0]
    uc.hook_add(UC_HOOK_INTR, on_intr)
    for off in (args.hook_installer, args.inline_installer):
        uc.hook_add(UC_HOOK_CODE, on_installer, begin=plug_base + off, end=plug_base + off)
    uc.hook_add(UC_HOOK_BLOCK, lambda uc, a, s, u: in_main.__setitem__(0, in_main[0] + 1), begin=main_base,
                end=main_base + up(mn.size))
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, lambda uc, access, a, size, value, u: print(
        f'unmapped memory access at {where(uc.reg_read(A.UC_ARM64_REG_PC))}: address {a:#x}', file=sys.stderr) or False)
    uc.reg_write(A.UC_ARM64_REG_SP, STACK + STACK_SIZE - 0x100)
    uc.reg_write(A.UC_ARM64_REG_TPIDRRO_EL0, TLS)
    uc.reg_write(A.UC_ARM64_REG_LR, STOP)
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)   # FP and SIMD on
    init = plug.dyn.get(12)   # DT_INIT
    if not init:
        sys.exit(f'{args.plugin} has no DT_INIT')
    uc.emu_start(plug_base + init, STOP + PAGE)

    changed = sum(1 for o in range(0, len(before) & ~3, 4) if before[o:o + 4] != bytes(bufs['main'][o:o + 4])) \
        if before != bytes(bufs['main']) else 0
    targets = []
    for t, _ in hooks:
        if t not in targets:
            targets.append(t)
    outside = [where(t) for t in targets + [t for t, _, _ in inlines] if not main_base <= t < main_base + mn.text[1]]
    if outside:
        sys.exit(f'hooks outside main\'s code, which the generator has no point for: {", ".join(outside)}')
    lines = [f'# {args.plugin} (build id {plug.build_id[:40]}) on main (build id {mn.build_id[:40]}): the functions and',
             '# instructions the plugin hooks when it starts (tools/plugin_hooks.py).',
             f'installer {args.plugin} hook {args.hook_installer:#x} {plug.u32(args.hook_installer):08x}',
             f'installer {args.plugin} inline {args.inline_installer:#x} {plug.u32(args.inline_installer):08x}']
    lines += [f'hook main {t - main_base:#x} {mn.u32(t - main_base):08x}' for t in sorted(targets)]
    lines += [f'inline main {t - main_base:#x} {mn.u32(t - main_base):08x}' for t, _, _ in inlines]
    text = '\n'.join(lines) + '\n'
    if args.out:
        args.out.write_text(text)
    else:
        sys.stdout.write(text)
    print(f'{len(hooks)} hooks of {len(targets)} functions, {len(inlines)} inline hooks; {len(log)} lines of debug output; '
          f'{in_main[0]} blocks of main executed, {changed} words of main changed (both expected 0)', file=sys.stderr)


if __name__ == '__main__':
    main()
