// A mod's plugin: a subsdk module of Switch code that a mod brings along (exefs/subsdk1..9), made with exlaunch.
//
// On the console such a plugin is loaded next to the game and rewrites the game's code in memory when it starts: it
// replaces the first instruction of a game function with a branch to its own function (a hook) and keeps a copy of
// that instruction followed by a branch back (the trampoline) to call the original. Lifted code cannot be rewritten
// while it runs, so the port builds the plugin in instead:
//
//   - recomp/layout.py places the plugin after the game's modules and recomp/gen.py lifts it like them;
//   - the functions and instructions it hooks are hook points of the generated C (recomp/patches.py `hook`, `inline`):
//     a hooked function enters through rt_hook_target[n], an inline point calls rt_inline_hook;
//   - the plugin's own two functions that install hooks are hook points too (`installer`), and this file fills them
//     with host_hook and host_inline: the plugin's start-up code runs unchanged, but a hook it asks for is recorded
//     here instead of written into code, and the trampoline it gets back is a host address that runs the function's
//     body (or the plugin function hooked there before);
//   - the plugin looks at its process through SVCs the game never makes: plugin_svc answers those the way Horizon
//     with Atmosphere does, for callers inside a plugin only. What the game sees does not change.
//
// The game's code in memory stays as shipped, which is also what the plugin checks its targets against.
//
// A plugin runs only while a mod that carries the same file is switched on (mods.c mods_plugin_on); otherwise its
// start-up code is never called, no hook point holds a target, and the game runs as without it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"

// runtime/patches.c, from the generated patches.h
extern const int rt_hook_count, rt_inline_count, rt_hook_installer, rt_inline_installer;
extern const uint64_t rt_hook_addr[], rt_inline_addr[];
extern const GuestFn rt_hook_orig[];
extern uint64_t rt_hook_target[];
extern uint8_t rt_inline_on[];

#define PAGE 0x1000ull
#define ST_FREE 0u
#define ST_CODE 3u
#define ST_CODEDATA 4u
#define ST_NORMAL 5u
#define ST_INACCESSIBLE 0x10u
#define PERM_R 1u
#define PERM_RW 3u
#define PERM_RX 5u
#define R_OK 0u
#define R_INVALID_STATE 0xd401u     // InvalidCurrentMemory
#define CUR_PROCESS 0xFFFF8001u     // Horizon's pseudo handle of the calling process

// Guest addresses this file owns. The trampolines are host functions in the upper half of the host-function window
// (hostfn.c fills it from the bottom, 16 bytes per function, and never gets that far: MAX_HOSTFN).
#define TRAMP_BASE (RT_HOSTFN_BASE + 0x80000ull)
#define TRAMP_MAX 256
// A plugin finds the modules of its process by walking the address space (a module is a run of code, read-only data
// and data), and takes the first for the dynamic linker and the second for the game's main. The port has no dynamic
// linker module, so the plugin is shown a stand-in of three pages just below main. It reads the stand-in's read-only
// page for the module's name; the pages are zero.
#define STANDIN_PAGES 3
#define RTLD_BASE (MOD_MAIN_BASE_ - STANDIN_PAGES * PAGE)
#define RTLD_RESERVE 0x10000ull     // below main: what the stand-in is reserved in (the OS reserves in 64 KiB steps)
// The room where a plugin maps a second, writable view of code (svc 0x74), above every region of the port. The
// plugin picks a random place in the address range GetInfo names and maps there when the range is free, so this
// range is the one it is told.
#define ALIAS_BASE 0x7100000000ull
#define ALIAS_SIZE 0x40000000ull
#define MAPS_MAX 16
#define INLINE_CHAIN 4

static uint64_t MOD_MAIN_BASE_;     // rt_layout[0].base: this file is in the runtime library, built without layout.h

typedef struct { uint64_t guest; GuestFn orig; } Tramp;   // where a trampoline goes: a guest address, else a body
static Tramp tramps[TRAMP_MAX];
static int ntramps;
typedef struct { uint64_t fn[INLINE_CHAIN]; uint8_t fp[INLINE_CHAIN]; int n; } Inline;   // newest last
static Inline *inlines;
typedef struct { uint64_t dst, src, size; } Map;
static Map maps[MAPS_MAX];
static int nmaps;
static PlatMutex lock = PLAT_MUTEX_INIT;
static int ready;                   // the stand-in and the mapping room are reserved, the installers are in place
static uint64_t autoload[2];        // nn::ro::detail::g_pAutoLoadList: an empty list (its head points at itself)
static uint32_t hooks_set, hooks_missing, inlines_set;

static const RtLoaded *plugin_at(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++)
        if (ldr_is_plugin(i) && a - ldr_module(i)->base < ldr_module(i)->size) return ldr_module(i);
    return NULL;
}

// ---- hooks

// A trampoline: the plugin's function calls it for "the function as it was before my hook".
static void tramp_entry(Cpu *c) {
    const Tramp *t = &tramps[(c->pc - TRAMP_BASE) >> 4];
    if (t->guest) {
        c->pc = t->guest;
        __attribute__((musttail)) return rt_dispatch(c);
    }
    __attribute__((musttail)) return t->orig(c);
}

static uint64_t tramp_new(uint64_t guest, GuestFn orig) {   // holding lock
    if (ntramps == TRAMP_MAX) rt_fatal("plugin: more than %d hooks", TRAMP_MAX);
    tramps[ntramps] = (Tramp){guest, orig};
    return TRAMP_BASE + 16ull * (uint64_t)ntramps++;
}

static int hook_point(uint64_t a) {
    for (int i = 0; i < rt_hook_count; i++)
        if (rt_hook_addr[i] == a) return i;
    return -1;
}

// The plugin's hook installer: hook(x0 function, x1 replacement, w2 wants a trampoline) -> x0 trampoline or 0.
// Hooks of one function chain as on the console: the newest replacement runs first and its trampoline leads to the
// one installed before it, the oldest one's to the function's body.
static void host_hook(Cpu *c) {
    uint64_t target = c->x[0], repl = c->x[1];
    int want = (int)(c->x[2] & 1), n = hook_point(target);
    char at[96], to[96], from[96];
    plat_mutex_lock(&lock);
    if (n < 0) {
        // Not built in: the function keeps running as shipped. The trampoline still has to work, so it is the
        // function itself.
        hooks_missing++;
        c->x[0] = want ? tramp_new(target, NULL) : 0;
        plat_mutex_unlock(&lock);
        rt_log("plugin: %s asks for a hook of %s that the game was built without; its function %s will not run "
               "(a `hook` line for it in the patch file and a rebuild add it)", ldr_describe(c->x[30] - 4, from, sizeof from),
               ldr_describe(target, at, sizeof at), ldr_describe(repl, to, sizeof to));
        return;
    }
    uint64_t before = __atomic_load_n(&rt_hook_target[n], __ATOMIC_ACQUIRE);
    c->x[0] = want ? tramp_new(before, before ? NULL : rt_hook_orig[n]) : 0;
    __atomic_store_n(&rt_hook_target[n], repl, __ATOMIC_RELEASE);
    hooks_set++;
    plat_mutex_unlock(&lock);
    if (rt_trace & RT_TRACE_HOSTCALL)
        rt_log("plugin: hook %s -> %s%s", ldr_describe(target, at, sizeof at), ldr_describe(repl, to, sizeof to),
               before ? " (chained)" : "");
}

// The plugin's inline hook installer: inline(x0 instruction, x1 function, w2 with the vector registers).
static void host_inline(Cpu *c) {
    uint64_t target = c->x[0], fn = c->x[1];
    int fp = (int)(c->x[2] & 1), n = -1;
    char at[96], to[96];
    for (int i = 0; i < rt_inline_count; i++)
        if (rt_inline_addr[i] == target) n = i;
    plat_mutex_lock(&lock);
    if (n < 0 || inlines[n].n == INLINE_CHAIN) {
        hooks_missing++;
        plat_mutex_unlock(&lock);
        rt_log("plugin: an inline hook at %s %s; its function %s will not run", ldr_describe(target, at, sizeof at),
               n < 0 ? "that the game was built without (an `inline` line in the patch file and a rebuild add it)"
                     : "beyond the hooks one instruction can hold", ldr_describe(fn, to, sizeof to));
        return;
    }
    Inline *in = &inlines[n];
    in->fn[in->n] = fn;
    in->fp[in->n] = (uint8_t)fp;
    __atomic_store_n(&in->n, in->n + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&rt_inline_on[n], 1, __ATOMIC_RELEASE);
    inlines_set++;
    plat_mutex_unlock(&lock);
    if (rt_trace & RT_TRACE_HOSTCALL)
        rt_log("plugin: inline hook %s -> %s%s", ldr_describe(target, at, sizeof at), ldr_describe(fn, to, sizeof to),
               fp ? " (with vector registers)" : "");
}

// An inline point of the generated C (recomp/gen.py), before its instruction, with every register in the Cpu.
// As exlaunch's stub does on the console: X30 goes on the stack, X0-X29 below it (0x100 bytes; with the vector
// registers, Q0-Q31 below those), the function gets the address of that block in X0 and may change what is in it, and
// the registers are loaded back from it. The stub keeps neither the flags nor, in its plain form, the vector
// registers; here both survive the plain form, which a hooked instruction cannot tell from a function that left them
// alone. The newest hook of an instruction runs first.
void rt_inline_hook(Cpu *c, int n) {
    const Inline *in = &inlines[n];
    for (int k = __atomic_load_n(&in->n, __ATOMIC_ACQUIRE) - 1; k >= 0; k--) {
        uint64_t sp = c->sp, lr = c->x[30], nzcv = c->nzcv, regs = sp - 0x110, block = in->fp[k] ? regs - 0x200 : regs;
        __uint128_t v[32];
        memcpy((void *)(uintptr_t)regs, c->x, 30 * 8);
        ST64(sp - 0x10, lr);
        if (in->fp[k]) memcpy((void *)(uintptr_t)block, c->v, sizeof c->v);
        else memcpy(v, c->v, sizeof v);
        c->sp = block;
        rt_call(c, in->fn[k], 1, block);
        memcpy(c->x, (const void *)(uintptr_t)regs, 30 * 8);
        if (in->fp[k]) memcpy(c->v, (const void *)(uintptr_t)block, sizeof c->v);
        else memcpy(c->v, v, sizeof v);
        c->sp = sp;
        c->x[30] = lr;
        c->nzcv = nzcv;
    }
}

// ---- the process as a plugin sees it (SVCs made from inside a plugin)

static uint64_t up(uint64_t v) { return (v + PAGE - 1) & ~(PAGE - 1); }

// The three regions of a module, or of a stand-in: code, read-only data, data.
static int in_module(uint64_t a, uint64_t base, uint64_t text, uint64_t data_off, uint64_t size, RtMemoryInfo *r) {
    if (a - base >= size) return 0;
    uint64_t off = a - base;
    if (off < text) *r = (RtMemoryInfo){.base = base, .size = text, .state = ST_CODE, .perm = PERM_RX};
    else if (off < data_off) *r = (RtMemoryInfo){.base = base + text, .size = data_off - text, .state = ST_CODE, .perm = PERM_R};
    else *r = (RtMemoryInfo){.base = base + data_off, .size = size - data_off, .state = ST_CODEDATA, .perm = PERM_RW};
    return 1;
}

static int in_real(uint64_t a, const RtLoaded *m, RtMemoryInfo *r) {
    return in_module(a, m->base, up(m->text_end), m->data_off, up(m->size), r);
}

// QueryMemory for a plugin. What the game gets (svc.c), with four differences:
//   - the dynamic linker's stand-in below main;
//   - every module as its three segments, whatever permissions the SDK has since given single data pages, so that a
//     module always reads as one;
//   - the mapping room, free but for what the plugin mapped there;
//   - memory of the host, which the game is told is code so that the SDK never maps there, is inaccessible instead:
//     a run of host memory followed by a module would read as a module that starts early.
static void query(uint64_t a, RtMemoryInfo *r) {
    if (in_module(a, RTLD_BASE, PAGE, 2 * PAGE, STANDIN_PAGES * PAGE, r)) return;
    for (int i = 0; i < ldr_count(); i++)
        if (in_real(a, ldr_module(i), r)) return;
    if (a - ALIAS_BASE < ALIAS_SIZE) {
        uint64_t lo = ALIAS_BASE, hi = ALIAS_BASE + ALIAS_SIZE;
        plat_mutex_lock(&lock);
        for (int i = 0; i < nmaps; i++) {
            const Map *m = &maps[i];
            if (a - m->dst < m->size) {
                *r = (RtMemoryInfo){.base = m->dst, .size = m->size, .state = ST_NORMAL, .perm = PERM_RW};
                plat_mutex_unlock(&lock);
                return;
            }
            if (m->dst + m->size <= a && m->dst + m->size > lo) lo = m->dst + m->size;
            if (m->dst > a && m->dst < hi) hi = m->dst;
        }
        plat_mutex_unlock(&lock);
        *r = (RtMemoryInfo){.base = lo, .size = hi - lo, .state = ST_FREE};
        return;
    }
    svc_query_memory(a, r);
    if (r->state == ST_CODE) {
        r->state = ST_INACCESSIBLE;
        r->perm = 0;
    }
    // the region ends where one of the pieces above begins (the game's answer knows the modules, not the other two)
    const uint64_t cut[][2] = {{RTLD_BASE, RTLD_BASE + STANDIN_PAGES * PAGE}, {ALIAS_BASE, ALIAS_BASE + ALIAS_SIZE}};
    uint64_t lo = r->base, hi = r->base + r->size;   // hi 0: up to the end of the address space
    for (unsigned i = 0; i < sizeof cut / sizeof *cut; i++) {
        if (cut[i][1] <= a && cut[i][1] > lo) lo = cut[i][1];
        if (cut[i][0] > a && (cut[i][0] < hi || !hi)) hi = cut[i][0];
    }
    r->base = lo;
    r->size = hi - lo;
}

// MapProcessMemory(x0 destination, w1 process, x2 source, x3 size): on the console a second view of the same pages,
// which the plugin writes code through. Here it is a copy: what the plugin stores in it reaches nothing, and the
// installers that would have stored hooks there never run.
static uint32_t map(uint64_t dst, uint64_t src, uint64_t size) {
    if (!size || ((dst | src | size) & (PAGE - 1)) || dst - ALIAS_BASE >= ALIAS_SIZE || size > ALIAS_BASE + ALIAS_SIZE - dst ||
        !plat_mem_readable(src, size))
        return R_INVALID_STATE;
    plat_mutex_lock(&lock);
    uint32_t r = nmaps == MAPS_MAX ? R_INVALID_STATE : R_OK;
    for (int i = 0; i < nmaps && !r; i++)
        if (dst < maps[i].dst + maps[i].size && maps[i].dst < dst + size) r = R_INVALID_STATE;
    if (!r && !plat_mem_commit(dst, size)) r = R_INVALID_STATE;
    if (!r) {
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
        maps[nmaps++] = (Map){dst, src, size};
    }
    plat_mutex_unlock(&lock);
    return r;
}

static uint32_t unmap(uint64_t dst, uint64_t size) {
    uint32_t r = R_INVALID_STATE;
    plat_mutex_lock(&lock);
    for (int i = 0; i < nmaps; i++)
        if (maps[i].dst == dst && maps[i].size == size) {
            plat_mem_decommit(dst, size);
            maps[i] = maps[--nmaps];
            r = R_OK;
            break;
        }
    plat_mutex_unlock(&lock);
    return r;
}

int plugin_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    const RtLoaded *self = ready ? plugin_at(pc) : NULL;
    if (!self) return 0;
    uint64_t *x = c->x;
    switch (imm) {
    case 0x06: {   // QueryMemory(x0 MemoryInfo*, x2 address -> w1 page info)
        RtMemoryInfo mi;
        query(x[2], &mi);
        if (x[0]) memcpy((void *)(uintptr_t)x[0], &mi, sizeof mi);
        x[0] = R_OK;
        x[1] = 0;
        return 1;
    }
    case 0x27: {   // OutputDebugString(x0 text, x1 size): a plugin's lines come without a line end; one log line each
        uint64_t n = x[1] > 1024 ? 1024 : x[1];
        const char *s = (const char *)(uintptr_t)x[0];
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
        if (n) rt_log("%s: %.*s", self->name, (int)n, s);
        x[0] = R_OK;
        return 1;
    }
    case 0x1E:     // GetSystemTick: the SDK reads the counter register instead; exlaunch seeds its random numbers
        x[0] = rt_ticks();
        return 1;
    case 0x29:     // GetInfo(w1 id, w2 handle, x3 sub id -> x1)
        switch ((uint32_t)x[1]) {
        case 12: x[1] = ALIAS_BASE; break;    // the address range to map in: the mapping room
        case 13: x[1] = ALIAS_SIZE; break;
        case 65001: x[1] = CUR_PROCESS; break;   // Atmosphere's extension: a handle of the calling process
        default: return 0;                    // svc.c answers the rest
        }
        x[0] = R_OK;
        return 1;
    case 0x74: x[0] = (uint32_t)x[1] == CUR_PROCESS ? map(x[0], x[2], x[3]) : R_INVALID_STATE; return 1;
    case 0x75: x[0] = (uint32_t)x[1] == CUR_PROCESS ? unmap(x[0], x[3]) : R_INVALID_STATE; return 1;
    }
    return 0;
}

// What a plugin stored into its copies of code since they were made (map): on the console those stores would have
// changed the code the game runs. The installers never run here, so there should be none; one that shows up names a
// way of patching the port does not build in yet.
static void lost_writes(const char *when) {
    plat_mutex_lock(&lock);
    for (int i = 0; i < nmaps; i++) {
        const uint8_t *a = (const uint8_t *)(uintptr_t)maps[i].dst, *b = (const uint8_t *)(uintptr_t)maps[i].src;
        if (!plat_mem_readable(maps[i].src, maps[i].size) || !memcmp(a, b, maps[i].size)) continue;
        uint64_t n = 0, first = 0;
        for (uint64_t k = 0; k < maps[i].size; k++)
            if (a[k] != b[k] && !n++) first = k;
        char at[96];
        rt_log("plugin: %s: %llu bytes the plugin wrote for %s (first at +%#llx) reach no code: the port builds hooks "
               "in instead of patching memory", when, (unsigned long long)n, ldr_describe(maps[i].src, at, sizeof at),
               (unsigned long long)first);
    }
    plat_mutex_unlock(&lock);
}

void plugin_exit_report(void) {
    if (!ready || !hooks_set) return;
    lost_writes("at exit");
    rt_log("plugin: %u hooks, %u inline hooks, %u refused", hooks_set, inlines_set, hooks_missing);
}

// ---- start

uint64_t plugin_data_symbol(const char *name) {
    if (strcmp(name, "_ZN2nn2ro6detail15g_pAutoLoadListE")) return 0;
    // The dynamic linker's list of modules. exlaunch walks it only to bind a function on its first call, which the
    // loader has done before any code runs; an empty list is a list head that points at itself.
    autoload[0] = autoload[1] = (uint64_t)(uintptr_t)autoload;
    return (uint64_t)(uintptr_t)autoload;
}

// Before hostfn_seal (boot.c): the trampoline addresses and the two installers join the dispatch table.
void plugin_init(void) {
    int plugins = 0;
    for (int i = 0; i < ldr_count(); i++) plugins += ldr_is_plugin(i);
    if (!plugins) return;
    MOD_MAIN_BASE_ = ldr_module(0)->base;
    if (rt_hook_installer < 0 || rt_inline_installer < 0) {
        rt_log("plugin: the game was built with a plugin module but without its hook points (no `installer` lines "
               "in the patch file); the plugin stays off");
        return;
    }
    if (!plat_mem_reserve(MOD_MAIN_BASE_ - RTLD_RESERVE, RTLD_RESERVE, 0) || !plat_mem_commit(RTLD_BASE, STANDIN_PAGES * PAGE) ||
        !plat_mem_reserve(ALIAS_BASE, ALIAS_SIZE, 0)) {
        rt_log("plugin: the address ranges below %#llx or at %#llx are taken; the plugin stays off",
               (unsigned long long)MOD_MAIN_BASE_, (unsigned long long)ALIAS_BASE);
        return;
    }
    inlines = calloc((size_t)rt_inline_count + 1, sizeof *inlines);
    if (!inlines) rt_fatal("plugin: inline hook table");
    for (int k = 0; k < TRAMP_MAX; k++) rt_register_host(TRAMP_BASE + 16ull * (uint64_t)k, tramp_entry);
    rt_hook_target[rt_hook_installer] = hostfn_register("<plugin hook installer>", host_hook);
    rt_hook_target[rt_inline_installer] = hostfn_register("<plugin inline hook installer>", host_inline);
    ready = 1;
}

// After the game's own module constructors (boot.c cb_module_inits), on the guest main thread: each plugin's DT_INIT,
// which is exlaunch's start-up code and then the plugin's own. The console's dynamic linker calls it at the same
// point, before nnMain.
void plugin_start(Cpu *c) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (!ldr_is_plugin(i)) continue;
        const char *force = cfg_get("TOTK_PLUGIN");   // 1: run it without a mod folder (tests), 0: never
        int on = force && *force ? strcmp(force, "0") != 0 : mods_plugin_on(m->name);
        if (!ready || !on || !m->init) {
            rt_log("plugin: %s is built in and stays off (%s)", m->name,
                   !ready ? "see above" : !m->init ? "no DT_INIT" : "no switched-on mod carries this file");
            continue;
        }
        uint32_t set = hooks_set, missing = hooks_missing, inl = inlines_set;
        rt_log("plugin: ENTER %s DT_INIT", m->name);
        rt_call(c, m->init, 0);
        rt_log("plugin: %s started: %u hooks and %u inline hooks in place%s", m->name, hooks_set - set, inlines_set - inl,
               hooks_missing != missing ? "; some were refused, see above" : "");
        lost_writes("after start-up");
    }
}
