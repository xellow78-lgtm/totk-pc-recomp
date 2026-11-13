"""The hook points of a mod's plugin in recomp/: the `hook`, `inline` and `installer` lines of the patch file
(recomp/patches.py), the plugin's place in the layout (recomp/layout.py), and what recomp/gen.py emits for them, on two
modules it makes itself: a main and a subsdk1 with a few two-instruction functions. No game files, no compiler: the
generated C is checked as text (runtime/plugin.c, which fills the points, has its checks in docs/building.md).

Run with .venv (lz4 and numpy, as recomp/): python tests/plugin_hooks_test.py
"""
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
import layout  # noqa: E402
import patches  # noqa: E402

MOV_W8_100, MOV_W8_200, RET = 0x52802008, 0x52804008, 0xD65F03C0
R_RELATIVE = 1027


def module(functions, bss=0x1000):
    """An uncompressed NSO: `b` over the MOD0 word, then the functions (lists of words) from 0x8 on; .rodata at 0x1000
    with MOD0 and one RELATIVE relocation per function (which makes each a function start for the generator); .data
    at 0x2000 with .dynamic and the relocated words. Returns (file bytes, [function offsets])."""
    words, starts = [0x14000002, 0x1100], []
    for f in functions:
        starts.append(4 * len(words))
        words += f
    text = struct.pack(f'<{len(words)}I', *words)
    ro = bytearray(0x400)
    ro[0x100:0x11C] = b'MOD0' + struct.pack('<6i', 0x2000 - 0x1100, 0, 0, -0x1100, -0x1100, 0)
    for i, s in enumerate(starts):
        ro[0x200 + 24 * i:0x218 + 24 * i] = struct.pack('<QQq', 0x2100 + 8 * i, R_RELATIVE, s)
    data = bytearray(0x200)
    dyn = [(7, 0x1200), (8, 24 * len(starts)), (0, 0)]
    data[0:16 * len(dyn)] = b''.join(struct.pack('<QQ', t, v) for t, v in dyn)
    segs = [(0x0, text), (0x1000, bytes(ro)), (0x2000, bytes(data))]
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    off = 0x100
    for i, (mem, d) in enumerate(segs):
        struct.pack_into('<IIII', hdr, 0x10 + 0x10 * i, off, mem, len(d), bss if i == 2 else 0)
        struct.pack_into('<I', hdr, 0x60 + 4 * i, len(d))
        off += len(d)
    return bytes(hdr) + b''.join(d for _, d in segs), starts


class PluginHooks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        (self.d / 'exefs').mkdir()
        # main: f (0x8), g (0x10, three instructions: the middle one gets the inline point), h (0x1c)
        main, self.main = module([[MOV_W8_100, RET], [MOV_W8_100, MOV_W8_200, RET], [MOV_W8_200, RET]])
        plug, self.plug = module([[MOV_W8_100, RET], [MOV_W8_200, RET]])
        (self.d / 'exefs' / 'main').write_bytes(main)
        (self.d / 'exefs' / 'subsdk1').write_bytes(plug)
        self.mods = layout.layout(self.d / 'exefs')

    def tearDown(self):
        self.tmp.cleanup()

    def table(self, text):
        (self.d / 'p.txt').write_text(text)
        return patches.load(self.d / 'p.txt', self.mods)

    def gen(self, text):
        (self.d / 'p.txt').write_text(text)
        return subprocess.run([sys.executable, str(ROOT / 'recomp' / 'gen.py'), '--exefs', str(self.d / 'exefs'),
                               '--out', str(self.d / 'gen'), '--patches', str(self.d / 'p.txt'), '--jobs', '1'],
                              capture_output=True, text=True)

    LINES = ('installer subsdk1 hook 0x8 52802008\n' 'installer subsdk1 inline 0x10 52804008\n'
             'hook main 0x8 52802008\n' 'hook main 0x1c 52804008\n' 'inline main 0x14 52804008\n')

    def test_a_plugin_goes_after_the_games_modules(self):
        (self.d / 'exefs' / 'sdk').write_bytes(module([[RET]])[0])
        names = [n for n, _, _ in layout.layout(self.d / 'exefs')]
        self.assertEqual(names, ['main', 'sdk', 'subsdk1'])
        (self.d / 'exefs' / 'subsdk0').write_bytes(module([[RET]])[0])
        (self.d / 'exefs' / 'subsdk7').write_bytes(module([[RET]])[0])
        mods = layout.layout(self.d / 'exefs')
        self.assertEqual([n for n, _, _ in mods], ['main', 'subsdk0', 'sdk', 'subsdk1', 'subsdk7'])
        # the game's bases are the ones a layout without plugins gives
        for n in ('subsdk1', 'subsdk7'):
            (self.d / 'exefs' / n).unlink()
        self.assertEqual(layout.layout(self.d / 'exefs'), mods[:3])

    def test_lines(self):
        t = self.table(self.LINES)
        self.assertEqual([(m, o) for m, o, _ in t.hooks], [('subsdk1', 0x8), ('subsdk1', 0x10), ('main', 0x8), ('main', 0x1c)])
        self.assertEqual([(m, o) for m, o, _ in t.inlines], [('main', 0x14)])
        self.assertEqual(t.installers, {'hook': 0, 'inline': 1})
        base = dict((n, b) for n, b, _ in self.mods)
        self.assertEqual(t.hooks_for('main', base['main']), {base['main'] + 0x8: 2, base['main'] + 0x1c: 3})
        h = patches.header_text(t, 'MARK', self.mods)
        self.assertIn('#define RT_HOOK_COUNT 4', h)
        self.assertIn(f'X(2, {base["main"] + 8:#x}ull, f_{base["main"] + 8:x}, f_{base["main"] + 8:x}_orig)', h)
        self.assertIn(f'#define RT_INLINES(X) X(0, {base["main"] + 0x14:#x}ull)', h)
        self.assertIn('#define RT_HOOK_INSTALLER 0', h)
        self.assertIn('#define RT_INLINE_INSTALLER 1', h)
        # a table without such lines writes the header it always wrote
        self.assertNotIn('RT_HOOK', patches.header_text(self.table(''), 'MARK', self.mods))

    def test_refused_lines(self):
        for bad, why in (('hook main 0x8 52802008\nhook main 0x8 52802008\n', 'twice'),
                         ('hook nomod 0x8 52802008\n', 'not in the layout'),
                         ('installer subsdk1 hook 0x8 52802008\ninstaller subsdk1 hook 0x10 52804008\n', 'second')):
            with self.assertRaisesRegex(patches.PatchError, why):
                self.table(bad)
        r = self.gen('hook main 0x8 52804008\n')          # the word at 0x8 is another
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('expects 52804008', r.stderr)
        r = self.gen('hook main 0x14 52804008\n')         # inside g: no function start
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no function start', r.stderr)

    def test_generated_code(self):
        r = self.gen(self.LINES)
        self.assertEqual(r.returncode, 0, r.stderr)
        base = dict((n, b) for n, b, _ in self.mods)
        c = (self.d / 'gen' / 'main' / 'main_0000.c').read_text()
        f, g, h = (base['main'] + o for o in self.main)
        # f and h: the body under f_<a>_orig, f_<a> goes through rt_hook_target; g keeps its name
        for a, n in ((f, 2), (h, 3)):
            self.assertRegex(c, rf'void f_{a:x}_orig\(Cpu \*c\) \{{\n    FN_ENTER')
            self.assertIn(f'void f_{a:x}(Cpu *c) {{\n    uint64_t to = rt_hook_target[{n}];\n'
                          f'    if (__builtin_expect(to != 0, 0)) {{ c->pc = to; __attribute__((musttail)) return '
                          f'rt_dispatch(c); }}\n    __attribute__((musttail)) return f_{a:x}_orig(c);\n}}', c)
        self.assertNotIn(f'f_{g:x}_orig', c)
        # g: the inline point sits between its first and its second instruction
        body = c[c.index(f'void f_{g:x}(Cpu *c)'):]
        body = body[:body.index('\n}') + 2]
        first, point, second = body.index('0x100'), body.index('rt_inline_hook(c, 0)'), body.index('0x200')
        self.assertTrue(first < point < second, body)
        self.assertIn('if (__builtin_expect(rt_inline_on[0], 0))', body)
        self.assertIn('extern uint64_t rt_hook_target[];', c)
        # the plugin's installers are hook points 0 and 1; the function table still names the hooked functions
        p = (self.d / 'gen' / 'subsdk1' / 'subsdk1_0000.c').read_text()
        self.assertIn('rt_hook_target[0]', p)
        self.assertIn('rt_hook_target[1]', p)
        self.assertIn(f'f_{f:x},', (self.d / 'gen' / 'main' / 'main_table.c').read_text() + ',')
        stats = re.search(r'"hook_points": (\d+),\s+"inline_points": (\d+)', (self.d / 'gen' / 'main' / 'manifest.json').read_text())
        self.assertEqual(stats.groups(), ('2', '1'))

    def test_without_lines_the_code_is_as_before(self):
        self.assertEqual(self.gen('').returncode, 0)
        plain = (self.d / 'gen' / 'main' / 'main_0000.c').read_text()
        self.assertNotIn('rt_hook_target', plain)
        self.assertNotIn('_orig', plain)
        # with the lines, only the hooked functions and the inline point differ: g's text is the plain one plus the point
        self.assertEqual(self.gen(self.LINES).returncode, 0)
        hooked = (self.d / 'gen' / 'main' / 'main_0000.c').read_text()
        strip = re.sub(r'    if \(__builtin_expect\(rt_inline_on\[0\], 0\)\) \{[^\n]*\}\n?', '', hooked)
        g = dict((n, b) for n, b, _ in self.mods)['main'] + self.main[1]
        cut = lambda t: t[t.index(f'void f_{g:x}(Cpu *c)'):][:t[t.index(f'void f_{g:x}(Cpu *c)'):].index('\n}')]
        self.assertEqual(cut(strip), cut(plain))


if __name__ == '__main__':
    unittest.main()
