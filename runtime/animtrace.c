// Link's animation slots as the player's equipment code drives them (TotK 1.4.3), read and, with one setting, changed.
//
// main+0x23ec64c is the helper that code uses while it draws, puts away or changes equipment: (x0 the player's
// equipment animation state, w1 the slot, x2 a pointer to the command's name pointer, w3 a number that is the same for
// a slot every time, w4 flags). Slot 0 is the upper body, 1 the right arm, 2 the lower body, 3 the left arm. A draw
// seen standing asks slot 0 for WeaponEquipOnUpper with flags 4 and from the next frame with flags 0, slot 2 for
// WeaponEquipOnLower (100, then 96) and slot 3 for WaitUpperInterpolation (4, then 0).
//
// anim_trace = 1 (TOTK_ANIM_TRACE): logs every request that differs from the slot's last one, with the time in
// milliseconds since the game started, and says how often and for how long the last one was repeated. So a clip of a
// draw can be read against what each slot was told and when, frame by frame, instead of inferring it from the picture.
//
// dual_left_draw = 1 (TOTK_DUAL_LEFT_DRAW, default 0), for the two-sword and hip-greatsword mods, whose left arm's draw
// is the first node of the WaitUpperInterpolation command in their Player.root.asb. The game asks the left-arm
// slot for that command only in its standing draw (main+0x23eccc4: slots 0, 2 and 3); the draw on the move
// (main+0x23ecbe0) asks slot 0 alone, and the left arm then shows what slot 3 was left with, the standing pose the
// command ends on, while the sword appears in the hand (seen in play and in this file's own trace). With the setting, every
// slot-0 request for WeaponEquipOnUpper also writes the slot-3 request the standing draw makes into the same request
// block, with the start flag when slot 0 has it: the name into the slot's buffer, 8 into its first flag byte, the
// flag into its second, exactly what the helper does (block + 8 + slot * 0x30: buffer pointer, then its size;
// block + 0xc8 + slot and block + 0xcc + slot: the two flag bytes). The standing draw's own request after it writes
// the same thing again.
#include "host.h"
#include <string.h>

#define REQUEST 0x23ec64cu       // stp x29, x30, [sp, #-0x50]!
#define SLOTS 16
#define START 4u                 // the flag seen on the first request of a draw

static struct { uint64_t name, lr, since, last; uint32_t a, b, n; char s[48]; } g_last[SLOTS];
static int g_trace, g_fix;

static uint64_t ms(void) { return (kern_now_ns() - rt_guest_start_ns) / 1000000u; }

static void left_arm_too(uint64_t block, uint32_t start) {
    static const char name[] = "WaitUpperInterpolation";
    uint64_t rec = block + 8 + 3 * 0x30;
    if (!mem_guest_readable(block, 0xd0)) return;
    uint64_t buf = LD64(rec);
    uint32_t cap = LD32(rec + 8);
    if (cap < sizeof name || !mem_guest_readable(buf, cap)) return;
    memcpy((void *)(uintptr_t)buf, name, sizeof name);
    ST8(block + 0xc8 + 3, LD8(block + 0xc8 + 3) | 8);
    ST8(block + 0xcc + 3, LD8(block + 0xcc + 3) | start);
    if (g_trace && start) rt_log("animtrace: %llu ms: slot 3 <- WaitUpperInterpolation (8, 4) written with the upper body's draw (dual_left_draw)", (unsigned long long)ms());
}

static void on_request(Cpu *c) {
    uint32_t slot = (uint32_t)c->x[1];
    uint64_t pp = c->x[2], name = 0, lr = c->x[30];
    if (pp && !(pp & 7) && mem_guest_readable(pp, 8)) name = LD64(pp);
    char s[48] = "(none)";
    if (name && mem_guest_readable(name, 1)) {
        int i = 0;
        while (i < 47 && mem_guest_readable(name + (uint64_t)i, 1) && LD8(name + (uint64_t)i)) s[i] = (char)LD8(name + (uint64_t)i), i++;
        s[i] = 0;
    }
    uint32_t a = (uint32_t)c->x[3], b = (uint32_t)c->x[4];
    if (g_fix && slot == 0 && !strcmp(s, "WeaponEquipOnUpper")) left_arm_too(c->x[0], b & START);
    if (!g_trace || slot >= SLOTS) return;
    uint64_t now = ms();
    if (g_last[slot].name == name && g_last[slot].lr == lr && g_last[slot].a == a && g_last[slot].b == b) { g_last[slot].n++, g_last[slot].last = now; return; }
    if (g_last[slot].n > 1)
        rt_log("animtrace:   (slot %u: %s (%u, %u) was asked %u times over %llu ms, last at %llu ms)", slot, g_last[slot].s, g_last[slot].a, g_last[slot].b, g_last[slot].n,
               (unsigned long long)(g_last[slot].last - g_last[slot].since), (unsigned long long)g_last[slot].last);
    g_last[slot].name = name, g_last[slot].lr = lr, g_last[slot].a = a, g_last[slot].b = b, g_last[slot].n = 1, g_last[slot].since = g_last[slot].last = now;
    memcpy(g_last[slot].s, s, sizeof s);
    rt_log("animtrace: %llu ms: slot %u <- %s (%u, %u) from main+%#llx", (unsigned long long)now, slot, s, a, b, (unsigned long long)(lr - ldr_module(0)->base));
}

void animtrace_init(void) {
    g_trace = rt_knob("ANIM_TRACE", "0"), g_fix = rt_knob("DUAL_LEFT_DRAW", "0");
    if (!g_trace && !g_fix) return;
    if (plugin_inline_host(REQUEST, on_request))
        rt_log("animtrace: %s%s", g_trace ? "logging the commands the player's equipment code asks Link's animation slots for" : "",
               g_fix ? (g_trace ? "; dual_left_draw is on" : "dual_left_draw is on") : "");
}
