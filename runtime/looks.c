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
//
// What the armor does stays the worn piece's. The pouch still holds the worn piece, but the game takes an armor's
// defense, set and effects from the actor on the player: main+0x4997c0 walks the player's three armor slots (x28 = 0,
// 1, 2), and for each reads the actor's ArmorParam (the pointer at +0x658 of the actor's armor part: BaseDefense at
// +0x110, SeriesName at +0x148, ActivateSetBonus at +0x16c, each behind a flag byte and a parent to ask when the flag
// is clear) and copies 0x120 bytes of effect data from +0x660 of that part into the player. Five inline points in that
// function hand it the worn piece's values instead: 0x49cc5c (w8, the defense read), 0x49cd00 and 0x49cf78 (w20 and
// w0, the set), 0x49cd90 (w8, the set bonus flag) and 0x49cde8 (x1, the source of the copy). The values are learned at
// the first of them, from every armor actor the function meets, under the name the actor itself carries, and kept in
// <profile>/looks-stats.bin. The name asked for last says nothing about the actor in the slot: an actor is made some
// frames after it is asked for, and the pouch screen asks before the world does. A worn piece whose values are not
// known yet is shown as itself (it is then learned, and shows the look the next time the slot's item changes).
// A sixth point, 0x49ce20, only logs what the function found for each object it ran for: a set with a bonus or none.
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
#define POINT_DEFENSE 0x49cc5cull  // in main+0x4997c0, see above
#define POINT_SERIES 0x49cd00ull
#define POINT_SERIES_FIRST 0x49cf78ull
#define POINT_SET_BONUS 0x49cd90ull
#define POINT_EFFECTS 0x49cde8ull
#define POINT_SET_RESULT 0x49ce20ull
#define NAME_CAP 64                // a fixed string's capacity, terminator included
#define SLOT_HEAD 9u
#define EFFECTS 0x120              // bytes of effect data an armor actor holds at +0x660
#define STATS_MAX 512
#define STATS_MAGIC 0x32534b4cu    // "LKS2": the first layout's files held values under wrong names and are not read
#define NAME_SCAN 0x400            // bytes of an actor searched for the pointer to its name

static const char *const SETTING[3] = {"TOTK_LOOK_HEAD", "TOTK_LOOK_UPPER", "TOTK_LOOK_LOWER"};
static const char *const SUFFIX[3] = {"_Head", "_Upper", "_Lower"};
static const char *const BARE[3] = {"Armor_999_Head", "Armor_Default_Upper", "Armor_Default_Lower"};   // an empty slot's piece
static char g_worn[3][NAME_CAP];   // the piece the game last asked for in each slot, before any look; "" bare
static int32_t g_dye[3] = {-1, -1, -1};   // and its dye (the makers' w3: the colour variation's index, -1 undyed)

// What an armor piece does, as main+0x4997c0 reads it from the piece's actor.
typedef struct {
    char name[NAME_CAP];
    int32_t defense;
    uint32_t series;             // ArmorParam's SeriesName as the game holds it
    uint8_t set_bonus, whole;    // whole: every field and the effect data were read
    uint8_t exact;               // read from the actor of this very name, not from a variant of it (Armor_..._Head_B)
    uint8_t effects[EFFECTS];
} Stat;
static Stat *g_stats;
static int g_nstats, g_stats_loaded;
static int g_at = -1;              // the slot main+0x4997c0 is reading with a look's actor in it, -1 otherwise
static const Stat *g_give;         // the worn piece's values to hand over for that slot
static int g_name_at = -1;         // where in an actor the pointer to its name is, once found (slot_name)

static void stats_path(char *out, size_t cap) {
    const char *dir = rt_config()->profile;
    if (dir && *dir) snprintf(out, cap, "%s/looks-stats.bin", dir);
    else *out = 0;
}
static void stats_load(void) {
    if (g_stats_loaded) return;
    g_stats_loaded = 1;
    g_stats = calloc(STATS_MAX, sizeof *g_stats);
    char path[1024];
    stats_path(path, sizeof path);
    FILE *f = g_stats && *path ? fopen(path, "rb") : NULL;
    if (!f) return;
    uint32_t head[3] = {0};
    if (fread(head, sizeof head, 1, f) == 1 && head[0] == STATS_MAGIC && head[1] == sizeof(Stat) && head[2] <= STATS_MAX &&
        fread(g_stats, sizeof(Stat), head[2], f) == head[2]) g_nstats = (int)head[2];
    fclose(f);
    for (int i = 0; i < g_nstats; i++) g_stats[i].name[NAME_CAP - 1] = 0;
    rt_log("looks: %d armor pieces known from %s", g_nstats, path);
}
static void stats_save(void) {
    char path[1024];
    stats_path(path, sizeof path);
    FILE *f = *path ? fopen(path, "wb") : NULL;
    if (!f) return;
    uint32_t head[3] = {STATS_MAGIC, sizeof(Stat), (uint32_t)g_nstats};
    fwrite(head, sizeof head, 1, f);
    fwrite(g_stats, sizeof(Stat), (size_t)g_nstats, f);
    fclose(f);
}
static Stat *stat_of(const char *name, int add) {
    stats_load();
    if (!g_stats || !*name) return NULL;
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return &g_stats[i];
    if (!add || g_nstats >= STATS_MAX) return NULL;
    Stat *s = &g_stats[g_nstats++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, NAME_CAP, "%s", name);
    return s;
}

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
    const char *worn = (const char *)(uintptr_t)LD64(c->x[2]);
    char look[NAME_CAP];
    int32_t dye = (int32_t)c->x[3];
    int shown = look_for(slot, worn, &dye, "world", look);
    slot -= SLOT_HEAD;
    if (slot > 2 || !worn || (*worn && !fits(worn, slot))) return;
    if (shown) {
        const Stat *st = stat_of(worn, 0);
        if (!st || !st->whole) {   // what the worn piece does is not known yet: it is shown itself, and learned
            static char told[NAME_CAP];
            if (strcmp(told, worn)) {
                snprintf(told, NAME_CAP, "%s", worn);
                rt_log("looks: what %s does is not known yet; it is shown itself this once", worn);
                if (strcmp(worn, BARE[slot])) menu_note("New piece under a look: take it off and put it back on once to show the look.");
            }
            shown = 0;
        }
    }
    if (!shown) return;
    memcpy(world[slot].name, look, NAME_CAP);
    world[slot].chars = world[slot].name;
    c->x[2] = (uint64_t)(uintptr_t)&world[slot];
    c->x[3] = (uint32_t)dye;
}

// The ArmorParam that holds a member itself: p, or the parent it names while its own flag byte is clear. As the game's
// inlined reads walk it (main+0x4997c0 at 0x49cf98), but without their type check of the parent, which is an
// ArmorParam's parent and so one itself; point_defense compares the defense it reaches with the game's own.
static uint64_t holder(uint64_t p, unsigned flag) {
    for (int i = 0; i < 8; i++) {
        if (LD8(p + flag) & 1 || !LD64(p + 0x10)) break;
        uint64_t h = LD64(p + 0x18);
        if (!h || LD32(h + 0xc) != LD32(p + 0x20) || !LD64(h)) break;
        p = LD64(h);
    }
    return p;
}

// The armor name at p, when p is one of this slot's: Armor_, letters, digits and underscores, with the slot's suffix
// in it (a head's variant made for some bodies carries more after it, as Armor_224_Head_B).
static int armor_string(uint64_t p, unsigned slot, char out[NAME_CAP]) {
    if (p < 0x10000 || !mem_guest_readable(p, NAME_CAP)) return 0;
    const char *t = (const char *)(uintptr_t)p;
    size_t n = strnlen(t, NAME_CAP);
    if (n >= NAME_CAP || strncmp(t, "Armor_", 6)) return 0;
    for (size_t k = 0; k < n; k++)
        if (!(t[k] == '_' || (t[k] >= '0' && t[k] <= '9') || (t[k] >= 'A' && t[k] <= 'Z') || (t[k] >= 'a' && t[k] <= 'z'))) return 0;
    memcpy(out, t, n + 1);
    return strstr(out, SUFFIX[slot]) != NULL;
}

// The name the actor in the slot main+0x4997c0 is reading carries, at its inline point 0x49cc5c. The function has the
// actor its slot's link leads to in the doubleword at x29-0x18 (0x49cc04) and took x25, the armor part, from it (the
// pointer at +0x110 of the table at actor+0x228); an actor that does not lead back to x25 is not read. Where an actor
// keeps the pointer to its name is found once: the first pointer in its first NAME_SCAN bytes to an armor name of the
// slot. LOOKS_TRACE lists every such pointer of the actors searched.
static int slot_name(Cpu *c, unsigned slot, char out[NAME_CAP]) {
    uint64_t actor = LD64(c->x[29] - 0x18);
    if (!actor || !mem_guest_readable(actor, NAME_SCAN)) return 0;
    uint64_t parts = LD64(actor + 0x228);
    if (!parts || !mem_guest_readable(parts, 0x118) || LD64(parts + 0x110) != c->x[25]) return 0;
    if (g_name_at >= 0) return armor_string(LD64(actor + (unsigned)g_name_at), slot, out);
    static int tries;
    if (tries >= 64) return 0;
    int trace = rt_knob("LOOKS_TRACE", "0"), found = 0;
    for (int at = 0; at < NAME_SCAN && (trace || !found); at += 8) {
        char name[NAME_CAP];
        if (!armor_string(LD64(actor + (unsigned)at), slot, name)) continue;
        if (trace) rt_log("looks:   slot %u's actor holds a pointer to \"%s\" at +0x%x", slot, name, (unsigned)at);
        if (!found++) g_name_at = at, memcpy(out, name, NAME_CAP);
    }
    if (found) rt_log("looks: an armor actor's name is at +0x%x (%s)", (unsigned)g_name_at, out);
    else if (++tries == 64) rt_log("looks: no armor actor carries a name the looks can read; what a piece does is not learned");
    return found != 0;
}

// The piece an actor's name stands for: the name up to the end of the slot's suffix.
static void piece_of(const char *name, unsigned slot, char out[NAME_CAP]) {
    const char *suffix = strstr(name, SUFFIX[slot]);
    size_t n = suffix ? (size_t)(suffix - name) + strlen(SUFFIX[slot]) : strlen(name);
    memcpy(out, name, n);
    out[n] = 0;
}

// Keeps what the actor named name (of piece, in slot) does: x25 its armor part, w8 the defense the game read from it.
// A variant's values do not replace those read from the piece's own actor.
static void learn(Cpu *c, unsigned slot, const char *name, const char *piece) {
    uint64_t param = LD64(c->x[25] + 0x658);
    int32_t defense = (int32_t)c->x[8];
    if (!param || (int32_t)LD32(holder(param, 0x174) + 0x110) != defense) {
        static int logged;
        if (!logged++) rt_log("looks: %s: the defense read here differs from the game's; the piece is not learned", name);
        return;
    }
    int exact = !strcmp(name, piece);
    Stat *st = stat_of(piece, 1), now;
    if (!st || (st->exact && !exact)) return;
    memcpy(&now, st, sizeof now);
    now.defense = defense;
    now.series = LD32(holder(param, 0x175) + 0x148);
    now.set_bonus = LD8(holder(param, 0x183) + 0x16c) != 0;
    now.whole = 1, now.exact = (uint8_t)exact;
    memcpy(now.effects, (const void *)(uintptr_t)(c->x[25] + 0x660), EFFECTS);
    if (!memcmp(&now, st, sizeof now)) return;
    // The same actor name with other values than before means a wrong read somewhere: it is said, not hidden.
    int again = st->whole && (st->defense != now.defense || st->series != now.series || st->set_bonus != now.set_bonus ||
                              memcmp(st->effects, now.effects, EFFECTS));
    memcpy(st, &now, sizeof now);
    stats_save();
    rt_log("looks: %s %s from the actor %s (slot %u): defense %d, set %u, set bonus %d", again ? "learned AGAIN, with other values," : "learned",
           piece, name, slot, (int)st->defense, (unsigned)st->series, (int)st->set_bonus);
    if (rt_knob("LOOKS_TRACE", "0"))   // the effect data in the log, for reading its layout
        for (int o = 0; o < EFFECTS; o += 32) {
            char hex[32 * 3 + 1];
            for (int k = 0; k < 32; k++) snprintf(hex + 3 * k, 4, "%02x ", st->effects[o + k]);
            rt_log("looks:   effects +%03x: %s", o, hex);
        }
}

// main+0x4997c0 has read a slot's defense into w8: x28 the slot, x25 the armor part of the slot's actor. The actor is
// learned; when it is the slot's look and not the worn piece, the worn piece's values go to the game from here on.
static void point_defense(Cpu *c) {
    // One line per change of what a slot shows and does. The function reads the slots of more than one object in a
    // frame, and two of them can hold different actors for as long as they exist: a state the slot had within its last
    // 120 reads is the other object's and is not logged again.
    static struct { char is[2 * NAME_CAP]; uint64_t at; } said[3][2];
    static uint64_t reads[3];
    g_at = -1, g_give = NULL;
    uint64_t i = c->x[28];
    if (i > 2) return;
    char name[NAME_CAP], piece[NAME_CAP], look[NAME_CAP], now[2 * NAME_CAP] = "";
    int32_t dye;
    if (!slot_name(c, (unsigned)i, name)) return;
    piece_of(name, (unsigned)i, piece);
    learn(c, (unsigned)i, name, piece);
    const char *worn = *g_worn[i] ? g_worn[i] : BARE[i];
    const Stat *st = NULL;
    if (strcmp(piece, worn) && setting((unsigned)i, look, &dye) && !strcmp(piece, look)) st = stat_of(worn, 0);
    if (st && st->whole) {
        g_at = (int)i, g_give = st;
        c->x[8] = (uint32_t)st->defense;
        snprintf(now, sizeof now, "%s>%s", name, worn);
    }
    if (!*now) snprintf(now, sizeof now, "%s", name);
    uint64_t at = ++reads[i];
    if (!strcmp(said[i][0].is, now)) said[i][0].at = at;
    else if (!strcmp(said[i][1].is, now) && at - said[i][1].at < 120) said[i][1].at = at;
    else {
        if (strcmp(said[i][1].is, now)) said[i][1] = said[i][0];
        memcpy(said[i][0].is, now, sizeof now), said[i][0].at = at;
        if (st && st->whole) rt_log("looks: slot %d shows %s, does what %s does: defense %d, set %u, set bonus %d", (int)i, name, worn,
                                    (int)st->defense, (unsigned)st->series, (int)st->set_bonus);
        else rt_log("looks: slot %d holds %s, worn %s: the game's own values", (int)i, name, worn);
    }
}
static void point_series(Cpu *c) {
    if (g_at >= 0 && g_give) c->x[20] = g_give->series;
}
static void point_series_first(Cpu *c) {
    if (g_at >= 0 && g_give) c->x[0] = g_give->series;
}
static void point_set_bonus(Cpu *c) {
    if (g_at >= 0 && g_give) c->x[8] = g_give->set_bonus;
}
// The copy of the slot's effect data into the player: x1 the source, +0x660 of the actor's armor part.
static void point_effects(Cpu *c) {
    if (g_at >= 0 && g_give) c->x[1] = (uint64_t)(uintptr_t)g_give->effects;
    g_at = -1, g_give = NULL;
}
// After the three slots: bit 0 of w19 says they make a set with a bonus, w21 which set, x26 whose armor it was; the
// game applies the bonus from there. Logged at every change, as the proof of what the points above led to. The
// function runs for more than one object in a frame (the log of a run without any look shows two that differ once the
// pouch screen has been open), so the last result is kept for each.
static void point_set_result(Cpu *c) {
    static struct { uint64_t who; int on; uint32_t set; } seen[8];
    static int n;
    int on = (int)(c->x[19] & 1), k = 0;
    uint32_t set = (uint32_t)c->x[21];
    while (k < n && seen[k].who != c->x[26]) k++;
    if (k == n) {
        if (n == 8) return;
        seen[n++].who = c->x[26], seen[k].on = -1;
    }
    if (seen[k].on == on && (!on || seen[k].set == set)) return;
    seen[k].on = on, seen[k].set = set;
    if (on) rt_log("looks: armor user %d: the game finds a set bonus, set %u", k, (unsigned)set);
    else rt_log("looks: armor user %d: the game finds no set bonus", k);
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
    int n = plugin_inline_host(POINT_GIVEN, point_given) + plugin_inline_host(POINT_POUCH, point_pouch);
    // The world's point only with the five that keep what the armor does the worn piece's
    int stats = plugin_inline_host(POINT_DEFENSE, point_defense) + plugin_inline_host(POINT_SERIES, point_series) +
                plugin_inline_host(POINT_SERIES_FIRST, point_series_first) + plugin_inline_host(POINT_SET_BONUS, point_set_bonus) +
                plugin_inline_host(POINT_EFFECTS, point_effects);
    if (stats == 5) n += plugin_inline_host(POINT_WORLD, point_world), plugin_inline_host(POINT_SET_RESULT, point_set_result);
    else if (any) rt_log("looks: the game was built with %d of the 5 points that keep an armor's effects the worn piece's; the look shows on the pouch screen only", stats);
    if (n) rt_log("looks: on, %d of 3 points built in%s", n, any ? "" : " (no look set)");
    else if (any) rt_log("looks: a look is set, but the game was built without the inline points");
}
