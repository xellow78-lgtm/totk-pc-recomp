"""Code patch table for the generator: guest instructions replaced in the lifted C, switchable at run time.

Lifted code cannot be byte-patched when the game runs, so a patch is applied when the C is generated. A patch file
(gen.py --patches) has two kinds of lines; `#` starts a comment, fields are separated by whitespace:

  set  <name> <env> <default>               a patch set: a run-time switch <env>=0/1, <default> when it is unset
  site <set> <module> <address> <original> <replacement> [<replacement> ...]

  spin <module> <head> <back-edge> [<back-edge> ...]

  hook   <module> <address> <original>      a function whose entry a mod's plugin may take over
  inline <module> <address> <original>      an instruction a mod's plugin may run its own code before
  installer <module> <hook|inline> <address> <original>

<address> is a module offset (hex), <original> the instruction word the module has there (checked; a mismatch stops
the generator), each <replacement> an instruction word (hex) or `@page:x<n>`, which is `adrp x<n>, <patch page>`
encoded for the site's address. The patch page is the first 4 KiB page after main's image (layout.py leaves the rest
of the 2 MiB-aligned module slot free); the runtime commits it and owns its contents. Words in use, by byte offset:
+0..15 the frame-rate step (runtime/frame_rate.c), +16..55 the output, swapchain and scene sizes
(runtime/resolution.c res_init), +56..59 the camera's aspect ratio (runtime/resolution.c res_init, set aspect), +60..63
the field-of-view factor (runtime/resolution.c res_fov_live, set fov), +92..95 the sun shadow map size
(runtime/resolution.c shadow_init, set shadow). A new set takes words outside these.

The replacement words are lifted at the site's address in place of the original instruction, and the generated C reads
`if (rt_patch_on[<set>]) { replacement } else { original }`: one byte load and a branch per executed site. Several
sets may patch one address (cheats that write different values there): `if (a) {..} else if (b) {..} else {..}`, the
first set in file order wins. The original may be any instruction but br, blr and ret; a replacement's last word may be
a bl (lifted with the site's return address), a b or b.cond / cbz / tbz (its target joins the function's labels when
inside it) or a ret; earlier words must not branch. Every alternative of a site writes the same flags. The generator
writes patches.h (set names, switches, the page address) next to layout.h for the runtime.

The `hook`, `inline` and `installer` lines serve a mod's plugin: a subsdk module of Switch code that, on the
console, rewrites the game's code in memory when it starts (an exlaunch plugin replaces a function's first instruction
with a branch to its own function, and keeps a copy of that instruction, the "trampoline", to call the original). The
plugin is lifted like the game's modules, and what it would have rewritten is built in here:
`hook` makes the function at <address> (checked: a function start) enter through `rt_hook_target[n]`: while the
runtime has stored a guest address there, a call of the function goes to that address, and the original body stays
reachable as f_<address>_orig (runtime/plugin.c hands it to the plugin as the trampoline). `inline` makes the
instruction at <address> call rt_inline_hook(c, n) first while rt_inline_on[n] is set: the runtime passes the
registers to the plugin's function and takes them back. `installer` names the plugin's own functions that install such
hooks on the console; it is a `hook` on them, which the runtime fills with its own function (it records the request
instead of writing code). n counts the lines of each kind in file order; patches.h lists them for the runtime.
A build without these lines has no hooks and its generated C is unchanged.

A `spin` line names a spin wait found by tools/spin_waits.py: a loop with head <head> that only another thread can end,
and the direct branches back to it (module offsets). Each back-edge (checked: a b, b.cond, cbz / cbnz or tbz / tbnz to
<head>) is lifted as `rt_spin_gate(c, <head>); goto <head>`, which keeps every register and only adds time
(runtime/kernel.c kern_spin_gate).
"""
from pathlib import Path

PAGE = 0x1000


class PatchError(ValueError):
    pass


def adrp(pc, target, rd):
    imm = (target >> 12) - (pc >> 12)
    if not -(1 << 20) <= imm < (1 << 20):
        raise PatchError(f'{pc:#x}: patch page {target:#x} out of ADRP range')
    imm &= 0x1FFFFF
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd


class Table:
    """sets: [(name, env, default)]; sites: {module: {offset: [(set index, [word | ('page', reg)], original)]}} (the
    alternatives of a site in file order); page."""

    def __init__(self, sets=(), sites=None, page=0, spins=None, hooks=(), inlines=(), installers=None):
        self.sets = list(sets)
        self.sites = sites or {}
        self.page = page
        self.spins = spins or {}   # {module: {back-edge offset: head offset}}
        self.hooks = list(hooks)       # [(module, offset, original word)] in file order: index n of rt_hook_target
        self.inlines = list(inlines)   # [(module, offset, original word)] in file order: index n of rt_inline_on
        self.installers = installers or {}   # {'hook' | 'inline': index into hooks}

    def hooks_for(self, name, base):
        """{absolute function start: hook index}."""
        return {base + off: n for n, (mod, off, _) in enumerate(self.hooks) if mod == name}

    def inlines_for(self, name, base):
        """{absolute instruction address: inline hook index}."""
        return {base + off: n for n, (mod, off, _) in enumerate(self.inlines) if mod == name}

    def spins_for(self, name, base):
        """{absolute back-edge address: absolute head address}."""
        return {base + b: base + h for b, h in self.spins.get(name, {}).items()}

    def for_module(self, name, base):
        """{absolute address: [(set index, [words])]} with @page resolved."""
        out = {}
        for off, alts in self.sites.get(name, {}).items():
            a = base + off
            out[a] = [(k, [adrp(a, self.page, w[1]) if isinstance(w, tuple) else w for w in repl])
                      for k, repl, _ in alts]
        return out


def load(path, mods):
    """Table from a patch file; mods is layout() output [(name, base, size)]."""
    sets, index, sites, spins = [], {}, {}, {}
    hooks, inlines, installers = [], [], {}

    def point(kind, mod, addr, orig):
        """Adds a hook or inline line; returns its index."""
        if mod not in bases:
            raise PatchError(f'{where}: module {mod} not in the layout')
        lst, key = (hooks if kind == 'hook' else inlines), (mod, int(addr, 16))
        if any((m, o) == key for m, o, _ in lst):
            raise PatchError(f'{where}: {mod}+{key[1]:#x} is a {kind} point twice')
        lst.append((mod, key[1], int(orig, 16)))
        return len(lst) - 1
    bases = {n: (b, s) for n, b, s in mods}
    for no, raw in enumerate(Path(path).read_text().splitlines(), 1):
        f = raw.split('#', 1)[0].split()
        if not f:
            continue
        where = f'{path}:{no}'
        if f[0] == 'set' and len(f) == 4:
            if f[1] in index:
                raise PatchError(f'{where}: set {f[1]} defined twice')
            index[f[1]] = len(sets)
            sets.append((f[1], f[2], int(f[3])))
        elif f[0] == 'site' and len(f) >= 6:
            if f[1] not in index:
                raise PatchError(f'{where}: unknown set {f[1]}')
            if f[2] not in bases:
                raise PatchError(f'{where}: module {f[2]} not in the layout')
            off, orig = int(f[3], 16), int(f[4], 16)
            repl = []
            for w in f[5:]:
                if w.startswith('@page:x'):
                    repl.append(('page', int(w[7:])))
                else:
                    repl.append(int(w, 16))
            alts = sites.setdefault(f[2], {}).setdefault(off, [])
            if any(k == index[f[1]] for k, _, _ in alts):
                raise PatchError(f'{where}: {f[2]}+{off:#x} patched twice by set {f[1]}')
            if alts and alts[0][2] != orig:
                raise PatchError(f'{where}: {f[2]}+{off:#x} expects {orig:08x}, an earlier line {alts[0][2]:08x}')
            alts.append((index[f[1]], repl, orig))
        elif f[0] in ('hook', 'inline') and len(f) == 4:
            point(f[0], f[1], f[2], f[3])
        elif f[0] == 'installer' and len(f) == 5 and f[2] in ('hook', 'inline'):
            if f[2] in installers:
                raise PatchError(f'{where}: a second {f[2]} installer')
            installers[f[2]] = point('hook', f[1], f[3], f[4])
        elif f[0] == 'spin' and len(f) >= 4:
            if f[1] not in bases:
                raise PatchError(f'{where}: module {f[1]} not in the layout')
            mod, head = spins.setdefault(f[1], {}), int(f[2], 16)
            for b in f[3:]:
                b = int(b, 16)
                if b in mod:
                    raise PatchError(f'{where}: {f[1]}+{b:#x} is a back-edge twice')
                mod[b] = head
        else:
            raise PatchError(f'{where}: cannot parse {raw!r}')
    base, size = bases['main'][0], bases['main'][1]
    page = (base + size + PAGE - 1) & ~(PAGE - 1)
    nxt = min((b for _, b, _ in mods if b > base), default=None)
    if nxt is not None and page + PAGE > nxt:
        raise PatchError(f'no room for the patch page after main ({page:#x})')
    return Table(sets, sites, page, spins, hooks, inlines, installers)


def check(table, mod, name):
    """Stops on a site whose original word differs from the module's."""
    for kind, lst in (('hook', table.hooks), ('inline', table.inlines)):
        for m, off, want in lst:
            if m != name:
                continue
            a = mod.base + off
            if not mod.in_text(a):
                raise PatchError(f'{name}+{off:#x}: {kind} point outside .text')
            if mod.word(a) != want:
                raise PatchError(f'{name}+{off:#x}: module has {mod.word(a):08x}, the {kind} line expects {want:08x}')
            if kind == 'inline' and (off in table.sites.get(name, {}) or off in table.spins.get(name, {})):
                raise PatchError(f'{name}+{off:#x}: an inline point on a patched site or a spin wait back-edge')
    for off, [(_, _, want), *_] in table.sites.get(name, {}).items():
        a = mod.base + off
        if not mod.in_text(a):
            raise PatchError(f'{name}+{off:#x}: patch site outside .text')
        have = mod.word(a)
        if have != want:
            raise PatchError(f'{name}+{off:#x}: module has {have:08x}, the patch expects {want:08x}')
    for b, head in table.spins.get(name, {}).items():
        a = mod.base + b
        if not mod.in_text(a) or branch_target(mod.word(a), a) != mod.base + head:
            raise PatchError(f'{name}+{b:#x}: not a direct branch to the spin wait head {name}+{head:#x}')


def branch_target(w, a):
    """Target of the direct branch w at a (b, b.cond, cbz / cbnz, tbz / tbnz), or None."""
    if w >> 26 == 0x05:
        return a + ((((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) << 2)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:
        return a + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000 << 2)
    if w & 0x7E000000 == 0x36000000:
        return a + ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000 << 2)
    return None


def header_text(table, marker, mods=()):
    """patches.h. mods: layout() output, for the absolute addresses of the hook and inline points."""
    lines = [f'// {marker}: code patch sets, generated by recomp/gen.py --patches. Do not edit or commit.',
             '#pragma once', '', f'#define RT_PATCH_COUNT {len(table.sets)}',
             f'#define RT_PATCH_PAGE {table.page:#x}ull',
             '// X(index, name, env switch, default)',
             '#define RT_PATCH_SETS(X) ' + ' '.join(f'X({i}, {n}, "{e}", {d})' for i, (n, e, d) in
                                                    enumerate(table.sets)), '']
    if table.hooks or table.inlines:
        base = {n: b for n, b, _ in mods}
        lines += ['// A mod plugin\'s hook points (recomp/patches.py `hook`, `inline`, `installer`; runtime/plugin.c).',
                  f'#define RT_HOOK_COUNT {len(table.hooks)}',
                  '// X(index, guest address, the function, the function without its hook check)',
                  '#define RT_HOOKS(X) ' + ' '.join(f'X({i}, {base[m] + o:#x}ull, f_{base[m] + o:x}, f_{base[m] + o:x}_orig)'
                                                    for i, (m, o, _) in enumerate(table.hooks)),
                  f'#define RT_INLINE_COUNT {len(table.inlines)}',
                  '// X(index, guest address)',
                  '#define RT_INLINES(X) ' + ' '.join(f'X({i}, {base[m] + o:#x}ull)'
                                                      for i, (m, o, _) in enumerate(table.inlines)),
                  f'#define RT_HOOK_INSTALLER {table.installers.get("hook", -1)}',
                  f'#define RT_INLINE_INSTALLER {table.installers.get("inline", -1)}', '']
    return '\n'.join(lines)
