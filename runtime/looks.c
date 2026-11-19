// Looks: the armor the player is shown in, apart from the armor worn. The settings look_head, look_upper and
// look_lower ([mods] in totk.ini; TOTK_LOOK_HEAD, ...) each name an armor actor, such as Armor_001_Head, to show in
// that slot in place of the worn piece; empty shows the worn piece. A slot with nothing worn keeps the game's default.
//
// TotK 1.4.3 only. Two functions make one piece of an equipment user's equipment from a slot number in w1 (9 head,
// 10 upper, 11 lower, as their compares against the default names Armor_999_Head, Armor_Default_Upper and
// Armor_Default_Lower show) and a string object in x2 whose first word points at the actor's name:
//   main+0xc48ac0 for the actors in the world. It only reads the name, so the inline point at its entry hands it
//   another string object, with the look's name.
//   main+0x1b7c3f4 for the actors of the pouch screen. It copies the name into a fixed string of 64 characters on its
//   stack and works from the copy; the inline point at main+0x1b7c4a4, after the copy, writes the look's name over it
//   (x21 the characters, w25 the slot). When its caller gave no name it takes the worn item's from the pouch; the
//   inline point at main+0x1b7cdc0 does the same there (the characters through the pointer at sp+0xd8, the slot in
//   the word at sp+0x78).
// Each point is an `inline main <offset> <word>` line in the patch file. Only a name of the player's armor in its own
// slot (Armor_..._Head, _Upper, _Lower) is replaced: other equipment users' pieces in these slots keep theirs.
// Both functions take the piece's dye in w3 (the index of its colour variation, -1 undyed; main+0x1b7c3f4 passes it
// on as the parameter ColorVariation_InitVariationIndex). A look carries its own dye: "<actor>:<dye>" in the setting.
// The pouch still holds the worn piece, and the damage the player takes follows it (seen in the game: with nothing
// worn and a look shown, a hit did the damage of no armor).
//
// Locking a look: while the pouch screen has the focus (menu_cursor.c), ZL or the L key makes the pieces worn at that moment the
// look of their slots, a bare slot included (the game's own piece for an empty slot), and saves the three settings;
// ZL again clears them. The game makes a piece only when the slot's item changes or a save loads, so after either
// press a slot shows the change the next time its item changes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "ui/menu.h"

#define POINT_WORLD 0xc48ac0ull    // main offsets of the inline points
#define POINT_GIVEN 0x1b7c4a4ull
#define POINT_POUCH 0x1b7cdc0ull
#define NAME_CAP 64                // a fixed string's capacity, terminator included
#define SLOT_HEAD 9u

static const char *const SETTING[3] = {"TOTK_LOOK_HEAD", "TOTK_LOOK_UPPER", "TOTK_LOOK_LOWER"};
static const char *const SUFFIX[3] = {"_Head", "_Upper", "_Lower"};
static const char *const BARE[3] = {"Armor_999_Head", "Armor_Default_Upper", "Armor_Default_Lower"};   // an empty slot's piece
static char g_worn[3][NAME_CAP];   // the piece the game last asked for in each slot, before any look; "" bare
static int32_t g_dye[3] = {-1, -1, -1};   // and its dye (the makers' w3: the colour variation's index, -1 undyed)

// 1 when name is an armor actor of slot (0 head, 1 upper, 2 lower), by its name. Whether the game has such an actor
// is not checked: a look it lacks leaves the slot empty or stops the game.
static int fits(const char *name, unsigned slot) {
    size_t n = strnlen(name, NAME_CAP), s = strlen(SUFFIX[slot]);
    return n < NAME_CAP && n > 6 + s && !strncmp(name, "Armor_", 6) && !strcmp(name + n - s, SUFFIX[slot]);
}

// A slot's setting, "<actor>" or "<actor>:<dye>", into name and *dye (-1 without one). 0 when it is empty.
static int setting(unsigned slot, char name[NAME_CAP], int32_t *dye) {
    const char *v = cfg_get(SETTING[slot]), *colon = v ? strchr(v, ':') : NULL;
    if (!v || !*v) return 0;
    snprintf(name, NAME_CAP, "%.*s", colon ? (int)(colon - v) : NAME_CAP - 1, v);
    *dye = colon ? (int32_t)strtol(colon + 1, NULL, 10) : -1;
    return 1;
}

// The look to show in place of worn (dyed *dye) in the game's slot number: 1 with the look's name in look and its dye
// in *dye, 0 for worn itself.
static int look_for(unsigned slot, const char *worn, int32_t *dye, const char *where, char look[NAME_CAP]) {
    slot -= SLOT_HEAD;
    if (slot > 2 || !worn) return 0;
    static int seen;
    if (seen < 40) seen++, rt_log("looks: %s asks for %s, dye %d (%s)", SUFFIX[slot] + 1, *worn ? worn : "no name", (int)*dye, where);
    if (!*worn || fits(worn, slot)) snprintf(g_worn[slot], NAME_CAP, "%s", worn), g_dye[slot] = *worn ? *dye : -1;
    int32_t want_dye;
    if (!setting(slot, look, &want_dye) || !fits(worn, slot) || !strncmp(worn, look, NAME_CAP)) return 0;
    static char logged[3][NAME_CAP];
    int first = strncmp(logged[slot], look, NAME_CAP - 1) != 0;
    if (first) snprintf(logged[slot], NAME_CAP, "%s", look);
    if (!fits(look, slot)) {
        if (first) rt_log("looks: %s = %s is not an armor name ending in %s; the worn piece is shown", SETTING[slot] + 5, look, SUFFIX[slot]);
        return 0;
    }
    if (first) rt_log("looks: %s shown as %s, dye %d", worn, look, (int)want_dye);
    *dye = want_dye;
    return 1;
}

// main+0x1b7c3f4 keeps its w3, the dye, in the word at x29-0xc (stored at its entry, before both of its points).
static void over(Cpu *c, unsigned slot, char *worn, const char *where) {
    char look[NAME_CAP];
    int32_t dye = (int32_t)LD32(c->x[29] - 0xc);
    if (!look_for(slot, worn, &dye, where, look)) return;
    memcpy(worn, look, strlen(look) + 1);
    ST32(c->x[29] - 0xc, (uint32_t)dye);
}

static void point_given(Cpu *c) { over(c, (unsigned)c->x[25], (char *)(uintptr_t)c->x[21], "pouch screen"); }
static void point_pouch(Cpu *c) { over(c, LD32(c->sp + 0x78), (char *)(uintptr_t)LD64(c->sp + 0xd8), "pouch screen, from the pouch"); }

// A string object as the game's code reads one, per slot: the pointer to the characters, then the characters.
static struct { const char *chars; char name[NAME_CAP]; } world[3];
static void point_world(Cpu *c) {
    unsigned slot = (unsigned)c->x[1];
    if (!c->x[2]) return;
    char look[NAME_CAP];
    int32_t dye = (int32_t)c->x[3];
    if (!look_for(slot, (const char *)(uintptr_t)LD64(c->x[2]), &dye, "world", look)) return;
    slot -= SLOT_HEAD;
    memcpy(world[slot].name, look, NAME_CAP);
    world[slot].chars = world[slot].name;
    c->x[2] = (uint64_t)(uintptr_t)&world[slot];
    c->x[3] = (uint32_t)dye;
}

// menu_cursor.c, at every pad read while the game's menus are followed: pouch is 1 while the pouch screen has the
// focus, buttons the pad's (HidNpadButton bits; ZL is bit 8, set for the L key too).
void looks_pouch(int pouch, uint64_t buttons) {
    static int held;
    int zl = (int)(buttons >> 8 & 1), press = zl && !held;
    held = zl;
    if (!pouch || !press) return;
    int set = 0;
    for (int i = 0; i < 3; i++) {
        const char *v = cfg_get(SETTING[i]);
        set |= v && *v;
    }
    for (int i = 0; i < 3; i++) {
        char v[NAME_CAP + 16] = "";
        if (!set) snprintf(v, sizeof v, "%s:%d", *g_worn[i] ? g_worn[i] : BARE[i], *g_worn[i] ? (int)g_dye[i] : -1);
        cfg_set(SETTING[i], v);
    }
    cfg_save();
    if (set) {
        rt_log("looks: cleared");
        menu_note("Look cleared. Each piece changes back the next time you change that slot.");
    } else {
        rt_log("looks: locked to %s, %s, %s", cfg_get(SETTING[0]), cfg_get(SETTING[1]), cfg_get(SETTING[2]));
        menu_note("Look locked to what you are wearing. Change into your real armor now. L or ZL here clears it.");
    }
}

void looks_init(void) {
    int any = 0;
    for (int i = 0; i < 3; i++) {
        const char *v = cfg_get(SETTING[i]);
        any |= v && *v;
    }
    int n = plugin_inline_host(POINT_WORLD, point_world) + plugin_inline_host(POINT_GIVEN, point_given) +
            plugin_inline_host(POINT_POUCH, point_pouch);
    if (n) rt_log("looks: on, %d of 3 points built in%s", n, any ? "" : " (no look set)");
    else if (any) rt_log("looks: a look is set, but the game was built without the inline points");
}
