// A greatsword's scabbard hangs where the greatsword does. The setting scabbard_back ([mods] in totk.ini;
// TOTK_SCABBARD_BACK, default 1) is for a mod that gives Link's equipment settings their own bone for greatswords
// (LSwordSheathedBoneName, not the one for swords).
//
// TotK 1.4.3 only. Two functions name the bone a put-away piece hangs on, from the wearer's EquipmentUserParam
// (SheathedBoneName at +0xe0, LSwordSheathedBoneName at +0xd8; the byte at +0x2e0 / +0x2e2 says the file itself sets
// the name, else the parent file's counts):
//   main+0xb586dc, for a weapon. Its main-weapon case (main+0xb58850) takes SheathedBoneName, asks the piece for its
//   weapon component and, when the weapon is a spear ([+0x77c] is 3) or a greatsword (2), takes the spear's or the
//   greatsword's name instead when that is not empty.
//   The function around main+0x601890, for a scabbard. For the main weapon's scabbard ([x19+0x50] is 3) it calls
//   main+0x603e38, which returns where SheathedBoneName is, and reads the name there (main+0x601ad0): the scabbard
//   never looks at the kind of weapon. (A SheathParam's BindBoneName is only read for a quiver, IsQuiver.)
// With the game's settings both names are the same bone; with a mod's they are not, and the scabbard stays behind on
// the sword's bone. Two inline points (`inline main` lines in the patch file):
//   WEAPON, the branch after the weapon component is asked for (cbz x0 at main+0xb588e8; x25 is the settings file the
//   name came from): the weapon's kind is remembered for that settings file.
//   SCABBARD, the read of the name (ldr x23, [x0] at main+0x601ad0): when the settings file x0 points into last saw a
//   greatsword and sets a greatsword bone of its own, x0 is pointed at that name instead.
#include "host.h"

#define WEAPON 0xb588e8u         // cbz x0, main+0xb58964
#define SCABBARD 0x601ad0u       // ldr x23, [x0]
#define FILES 8                  // settings files remembered: Link's and the inventory Link's, with room to spare
#define KIND_LSWORD 2
#define SHEATH_MAIN 3

static struct { uint64_t file; uint32_t kind; } g_last[FILES];
static int g_next, g_trace;
static uint64_t g_said;          // the last (file, outcome) logged, so that a scabbard read every frame is said once

static int readable(uint64_t a, uint64_t n) { return a && !(a & 3) && mem_guest_readable(a, n); }

static void point_weapon(Cpu *c) {
    uint64_t file = c->x[25], weapon = c->x[0];
    if (!weapon || !readable(weapon, 0x780) || !readable(file, 0x2e8)) return;
    uint32_t kind = LD32(weapon + 0x77c);
    int i = 0;
    while (i < FILES && g_last[i].file != file) i++;
    if (i == FILES) i = g_next, g_next = (g_next + 1) % FILES, g_last[i].file = file, g_last[i].kind = ~0u;
    if (g_trace && g_last[i].kind != kind) rt_log("scabbard: settings %#llx: the main weapon is of kind %u", (unsigned long long)file, kind);
    g_last[i].kind = kind;
}

static void point_scabbard(Cpu *c) {
    uint64_t sheath = c->x[19], at = c->x[0];
    if (!readable(sheath, 0x58) || LD32(sheath + 0x50) != SHEATH_MAIN || at < 0xe0) return;
    uint64_t file = at - 0xe0;
    int i = 0;
    while (i < FILES && g_last[i].file != file) i++;
    if (i == FILES) return;      // not a settings file a weapon went through (the fallback string, or nothing held yet)
    int moved = 0;
    if (g_last[i].kind == KIND_LSWORD && readable(file, 0x2e8) && (LD8(file + 0x2e2) & 1)) {
        uint64_t name = LD64(file + 0xd8);
        if (name && mem_guest_readable(name, 1) && LD8(name)) c->x[0] = file + 0xd8, moved = 1;
    }
    if (g_trace && g_said != (file ^ (uint64_t)moved << 63 ^ sheath)) {
        g_said = file ^ (uint64_t)moved << 63 ^ sheath;
        rt_log("scabbard: scabbard %#llx of settings %#llx (weapon kind %d): %s", (unsigned long long)sheath, (unsigned long long)file, (int)g_last[i].kind,
               moved ? "on the greatsword's bone" : "on the sword's bone");
    }
}

void scabbard_init(void) {
    if (!rt_knob("SCABBARD_BACK", "1")) return;
    g_trace = rt_knob("SCABBARD_TRACE", "0");
    int n = plugin_inline_host(WEAPON, point_weapon) + plugin_inline_host(SCABBARD, point_scabbard);
    if (n) rt_log("scabbard: a greatsword's scabbard follows the greatsword (%d of 2 points)%s", n, g_trace ? ", traced" : "");
}
