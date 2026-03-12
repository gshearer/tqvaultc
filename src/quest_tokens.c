/**
 * quest_tokens.c — QuestToken.myw parser/writer and quest definition table
 *
 * Binary format:
 *   [len-prefixed "versionNumber"][u32: 1]
 *   [len-prefixed "begin_block"][0xCEFA1DB0]
 *   [len-prefixed "numberOfTriggerTokens"][u32: count]
 *     // Repeated `count` times:
 *     [len-prefixed "begin_block"][0xCEFA1DB0]
 *     [len-prefixed "name"][len-prefixed token_string]
 *     [len-prefixed "fileReferenceCount"][u32: 0]
 *     [len-prefixed "end_block"][0xDEADC0DE]
 *   [len-prefixed "end_block"][0xDEADC0DE]
 */

#include "quest_tokens.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

/* ── ByteBuf: growable byte buffer ────────────────────────────────────── */

typedef struct { uint8_t *data; size_t size; size_t cap; } ByteBuf;

static void bb_init(ByteBuf *b, size_t cap) {
    b->data = malloc(cap);
    b->size = 0;
    b->cap  = cap;
}

static void bb_ensure(ByteBuf *b, size_t need) {
    if (b->size + need <= b->cap) return;
    while (b->cap < b->size + need) b->cap *= 2;
    b->data = realloc(b->data, b->cap);
}

static void bb_write(ByteBuf *b, const void *src, size_t len) {
    bb_ensure(b, len);
    memcpy(b->data + b->size, src, len);
    b->size += len;
}

static void bb_write_u32(ByteBuf *b, uint32_t val) {
    bb_write(b, &val, 4);
}

static void bb_write_str(ByteBuf *b, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    bb_write_u32(b, len);
    if (len > 0) bb_write(b, s, len);
}

/* ── Binary reader helpers ────────────────────────────────────────────── */

static uint32_t read_u32(const uint8_t *data, size_t off) {
    uint32_t val;
    memcpy(&val, data + off, 4);
    return val;
}

/* Read a length-prefixed string; advances *off. Returns malloc'd string or NULL. */
static char *read_str(const uint8_t *data, size_t len, size_t *off) {
    if (*off + 4 > len) return NULL;
    uint32_t slen = read_u32(data, *off);
    *off += 4;
    if (slen == 0 || *off + slen > len) return NULL;
    char *s = malloc(slen + 1);
    memcpy(s, data + *off, slen);
    s[slen] = '\0';
    *off += slen;
    return s;
}

/* Skip a length-prefixed string key and return true if it matches expected. */
static bool expect_key(const uint8_t *data, size_t len, size_t *off, const char *expected) {
    char *key = read_str(data, len, off);
    if (!key) return false;
    bool match = strcmp(key, expected) == 0;
    free(key);
    return match;
}

/* ── Token set operations ─────────────────────────────────────────────── */

void quest_token_set_init(QuestTokenSet *set) {
    set->tokens = NULL;
    set->count = 0;
    set->capacity = 0;
    set->dirty = false;
}

void quest_token_set_free(QuestTokenSet *set) {
    for (int i = 0; i < set->count; i++)
        free(set->tokens[i]);
    free(set->tokens);
    set->tokens = NULL;
    set->count = 0;
    set->capacity = 0;
}

bool quest_token_set_contains(const QuestTokenSet *set, const char *token) {
    for (int i = 0; i < set->count; i++)
        if (strcmp(set->tokens[i], token) == 0) return true;
    return false;
}

void quest_token_set_add(QuestTokenSet *set, const char *token) {
    if (quest_token_set_contains(set, token)) return;
    if (set->count >= set->capacity) {
        set->capacity = set->capacity ? set->capacity * 2 : 64;
        set->tokens = realloc(set->tokens, set->capacity * sizeof(char *));
    }
    set->tokens[set->count++] = strdup(token);
    set->dirty = true;
}

void quest_token_set_remove(QuestTokenSet *set, const char *token) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->tokens[i], token) == 0) {
            free(set->tokens[i]);
            set->tokens[i] = set->tokens[--set->count];
            set->dirty = true;
            return;
        }
    }
}

/* ── Parser ───────────────────────────────────────────────────────────── */

int quest_tokens_load(const char *filepath, QuestTokenSet *out) {
    quest_token_set_init(out);

    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return -1; }
    rewind(f);

    uint8_t *data = malloc(fsize);
    if (fread(data, 1, fsize, f) != (size_t)fsize) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    size_t off = 0;
    size_t len = (size_t)fsize;

    /* versionNumber + value */
    if (!expect_key(data, len, &off, "versionNumber")) { free(data); return -1; }
    if (off + 4 > len) { free(data); return -1; }
    off += 4; /* skip version value (1) */

    /* begin_block + marker */
    if (!expect_key(data, len, &off, "begin_block")) { free(data); return -1; }
    if (off + 4 > len) { free(data); return -1; }
    off += 4; /* skip 0xCEFA1DB0 */

    /* numberOfTriggerTokens + count */
    if (!expect_key(data, len, &off, "numberOfTriggerTokens")) { free(data); return -1; }
    if (off + 4 > len) { free(data); return -1; }
    uint32_t count = read_u32(data, off);
    off += 4;

    out->capacity = count > 0 ? (int)count : 64;
    out->tokens = malloc(out->capacity * sizeof(char *));

    for (uint32_t i = 0; i < count; i++) {
        /* begin_block + marker */
        if (!expect_key(data, len, &off, "begin_block")) break;
        if (off + 4 > len) break;
        off += 4;

        /* name + token string */
        if (!expect_key(data, len, &off, "name")) break;
        char *token = read_str(data, len, &off);
        if (!token) break;

        out->tokens[out->count++] = token;

        /* fileReferenceCount + value */
        if (!expect_key(data, len, &off, "fileReferenceCount")) break;
        if (off + 4 > len) break;
        off += 4;

        /* end_block + marker */
        if (!expect_key(data, len, &off, "end_block")) break;
        if (off + 4 > len) break;
        off += 4;
    }

    free(data);
    out->dirty = false;
    return 0;
}

/* ── Writer ───────────────────────────────────────────────────────────── */

int quest_tokens_save(const char *filepath, const QuestTokenSet *set) {
    quest_backup_file(filepath);

    ByteBuf bb;
    bb_init(&bb, 65536);

    /* Header */
    bb_write_str(&bb, "versionNumber");
    bb_write_u32(&bb, 1);
    bb_write_str(&bb, "begin_block");
    bb_write_u32(&bb, 0xB01DFACE);  /* 0xCEFA1DB0 LE = 0xB01DFACE as written */
    bb_write_str(&bb, "numberOfTriggerTokens");
    bb_write_u32(&bb, (uint32_t)set->count);

    /* Token entries */
    for (int i = 0; i < set->count; i++) {
        bb_write_str(&bb, "begin_block");
        bb_write_u32(&bb, 0xB01DFACE);
        bb_write_str(&bb, "name");
        bb_write_str(&bb, set->tokens[i]);
        bb_write_str(&bb, "fileReferenceCount");
        bb_write_u32(&bb, 0);
        bb_write_str(&bb, "end_block");
        bb_write_u32(&bb, 0xDEADC0DE);
    }

    /* Outer end_block */
    bb_write_str(&bb, "end_block");
    bb_write_u32(&bb, 0xDEADC0DE);

    FILE *f = fopen(filepath, "wb");
    if (!f) { free(bb.data); return -1; }
    size_t written = fwrite(bb.data, 1, bb.size, f);
    fclose(f);
    free(bb.data);

    return (written == bb.size) ? 0 : -1;
}

/* ── Path helper ──────────────────────────────────────────────────────── */

static const char *diff_dirs[] = { "Normal", "Epic", "Legendary" };

char *quest_token_path(const char *char_filepath, QuestDifficulty diff) {
    if (!char_filepath || diff < 0 || diff >= NUM_DIFFICULTIES) return NULL;

    /* char_filepath is e.g. .../SaveData/Main/_soothie/Player.chr
     * We need: .../SaveData/Main/_soothie/Levels_World_World01.map/{difficulty}/QuestToken.myw */
    char *dir = g_path_get_dirname(char_filepath);
    char *path = g_build_filename(dir, "Levels_World_World01.map",
                                  diff_dirs[diff], "QuestToken.myw", NULL);
    g_free(dir);
    return path;
}

char *quest_state_dir(const char *char_filepath, QuestDifficulty diff) {
    if (!char_filepath || diff < 0 || diff >= NUM_DIFFICULTIES) return NULL;

    char *dir = g_path_get_dirname(char_filepath);
    char *path = g_build_filename(dir, "Levels_World_World01.map",
                                  diff_dirs[diff], NULL);
    g_free(dir);
    return path;
}

/* ── Backup helper ────────────────────────────────────────────────────── */

int quest_backup_file(const char *filepath) {
    if (!g_file_test(filepath, G_FILE_TEST_EXISTS))
        return 0;  /* nothing to backup */

    char bak[1024];
    snprintf(bak, sizeof(bak), "%s.bak", filepath);

    FILE *src = fopen(filepath, "rb");
    FILE *dst = fopen(bak, "wb");
    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        return -1;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);

    fclose(src);
    fclose(dst);
    return 0;
}

/* ── Copy file helper ─────────────────────────────────────────────────── */

static int copy_file(const char *src_path, const char *dst_path) {
    FILE *src = fopen(src_path, "rb");
    if (!src) return -1;

    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { fclose(src); return -1; }

    char buf[4096];
    size_t n;
    int err = 0;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { err = -1; break; }
    }

    fclose(src);
    fclose(dst);
    return err;
}

/* ── Quest state file operations ──────────────────────────────────────── */

int quest_que_clear_all(const char *quest_dir) {
    GDir *d = g_dir_open(quest_dir, 0, NULL);
    if (!d) return -1;

    int modified = 0;
    const gchar *ent_name;
    while ((ent_name = g_dir_read_name(d)) != NULL) {
        size_t nlen = strlen(ent_name);
        if (nlen < 5 || strcmp(ent_name + nlen - 4, ".que") != 0)
            continue;

        char *filepath = g_build_filename(quest_dir, ent_name, NULL);

        /* Read entire file */
        FILE *f = fopen(filepath, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        if (fsize <= 0) { fclose(f); continue; }
        rewind(f);

        uint8_t *data = malloc(fsize);
        if (fread(data, 1, fsize, f) != (size_t)fsize) {
            free(data); fclose(f); continue;
        }
        fclose(f);

        /* Scan for hasFired and isPendingFire keys and zero their values */
        bool changed = false;
        static const struct { const char *key; size_t klen; } targets[] = {
            { "hasFired",       8  },
            { "isPendingFire",  13 },
        };

        for (int t = 0; t < 2; t++) {
            const char *key = targets[t].key;
            size_t klen = targets[t].klen;

            /* Pattern: u32 length prefix == klen, then key bytes, then u32 value */
            for (size_t off = 0; off + 4 + klen + 4 <= (size_t)fsize; off++) {
                uint32_t slen;
                memcpy(&slen, data + off, 4);
                if (slen != (uint32_t)klen) continue;
                if (memcmp(data + off + 4, key, klen) != 0) continue;

                /* Found it — zero the u32 value that follows */
                size_t val_off = off + 4 + klen;
                uint32_t val;
                memcpy(&val, data + val_off, 4);
                if (val != 0) {
                    uint32_t zero = 0;
                    memcpy(data + val_off, &zero, 4);
                    changed = true;
                }
                off = val_off + 3; /* skip past this match */
            }
        }

        if (changed) {
            quest_backup_file(filepath);
            f = fopen(filepath, "wb");
            if (f) {
                fwrite(data, 1, fsize, f);
                fclose(f);
                modified++;
            }
        }
        free(data);
        g_free(filepath);
    }
    g_dir_close(d);
    return modified;
}

int quest_myw_clear(const char *quest_dir) {
    char *filepath = g_build_filename(quest_dir, "Quest.myw", NULL);

    quest_backup_file(filepath);

    /* Write minimal Quest.myw:
     * begin_block + CEFA1DB0 + numberOfTriggers(0) + end_block + DEADC0DE
     * begin_block + CEFA1DB0 + numRewards(0) + end_block + DEADC0DE */
    ByteBuf bb;
    bb_init(&bb, 256);

    /* Triggers section (empty) */
    bb_write_str(&bb, "begin_block");
    bb_write_u32(&bb, 0xB01DFACE);
    bb_write_str(&bb, "numberOfTriggers");
    bb_write_u32(&bb, 0);
    bb_write_str(&bb, "end_block");
    bb_write_u32(&bb, 0xDEADC0DE);

    /* Rewards section (empty) */
    bb_write_str(&bb, "begin_block");
    bb_write_u32(&bb, 0xB01DFACE);
    bb_write_str(&bb, "numRewards");
    bb_write_u32(&bb, 0);
    bb_write_str(&bb, "end_block");
    bb_write_u32(&bb, 0xDEADC0DE);

    FILE *f = fopen(filepath, "wb");
    if (!f) { free(bb.data); g_free(filepath); return -1; }
    size_t written = fwrite(bb.data, 1, bb.size, f);
    fclose(f);
    free(bb.data);
    g_free(filepath);

    return (written == bb.size) ? 0 : -1;
}

int quest_copy_state_from(const char *src_dir, const char *dst_dir) {
    GDir *d = g_dir_open(src_dir, 0, NULL);
    if (!d) return -1;

    int errors = 0;
    const gchar *ent_name;
    while ((ent_name = g_dir_read_name(d)) != NULL) {
        size_t nlen = strlen(ent_name);
        bool is_que = (nlen >= 5 && strcmp(ent_name + nlen - 4, ".que") == 0);
        bool is_quest_myw = (strcmp(ent_name, "Quest.myw") == 0);
        if (!is_que && !is_quest_myw) continue;

        char *src_path = g_build_filename(src_dir, ent_name, NULL);
        char *dst_path = g_build_filename(dst_dir, ent_name, NULL);

        quest_backup_file(dst_path);
        if (copy_file(src_path, dst_path) != 0)
            errors++;
        g_free(src_path);
        g_free(dst_path);
    }
    g_dir_close(d);
    return errors == 0 ? 0 : -1;
}

/* ── Name helpers ─────────────────────────────────────────────────────── */

const char *quest_act_name(QuestAct act) {
    static const char *names[] = {
        "Greece", "Egypt", "Orient", "Immortal Throne",
        "Ragnarok", "Atlantis", "Eternal Embers"
    };
    if (act >= 0 && act < NUM_ACTS) return names[act];
    return "Unknown";
}

const char *quest_difficulty_name(QuestDifficulty diff) {
    if (diff >= 0 && diff < NUM_DIFFICULTIES) return diff_dirs[diff];
    return "Unknown";
}

/* ── Static quest definition table ────────────────────────────────────── */
/*
 * Organized by act. Each quest lists all tokens it triggers, plus the
 * "completion token" used to determine checkbox state.
 *
 * Token assignments derived from analysis of a fully-completed Legendary
 * character with 621 tokens.
 */

/* --- ACT I: Greece --------------------------------------------------- */

static const char *q_greece_horse[] = {
    "Q1_HorseSaved", "Q1_HorsePartCompleted", NULL
};
static const char *q_greece_shaman[] = {
    "Q1_DialogTabChanged", "Q1_ShamanKilled", "BossChest_SatyrShaman",
    "Q1_ShamanPartCompleted", NULL
};
static const char *q_greece_nessus[] = {
    "Q2_NessusKilled", "BossChest_Nessus", NULL
};
static const char *q_greece_telkine[] = {
    "Greece - Telkine Defeated", NULL
};

/* Greece side quests */
static const char *q_greece_jg01[] = {
    "JG01 - AssignedShepherdTrapped", NULL
};
static const char *q_greece_jg02[] = {
    "JG02 - KilledMonsters", NULL
};
static const char *q_greece_jg03[] = {
    "JG03 - AssignedRoadBlock", "JG03 - KilledBrute", NULL
};
static const char *q_greece_jg04[] = {
    "JG04 - AssignedFindDowry", NULL
};
static const char *q_greece_jg05[] = {
    "JG05 - AssignedFindOldSoldier", "JG05 - FoundOldSoldier", NULL
};
static const char *q_greece_jg06[] = {
    "JG06 - AssignedPoisonSpring", "JG06 - KilledSpiders", NULL
};
static const char *q_greece_jg07[] = {
    "JG07 - AssignedUndeadRaiders", "JG07 - KilledThreeBrothers", NULL
};
static const char *q_greece_jg09[] = {
    "JG09 - AssignedFindShipwreck", NULL
};
static const char *q_greece_jg10[] = {
    "JG10 - TalkedToChiron", NULL
};
static const char *q_greece_jg11[] = {
    "JG11 - KilledFlamingBoarMan", NULL
};
static const char *q_greece_jg12[] = {
    "JG12 - KilledGiantLimos", NULL
};
static const char *q_greece_jg13[] = {
    "JG13 - FoundLostScout", NULL
};
static const char *q_greece_jg14[] = {
    "JG14 - ArachneKilled", "JG14 - FireResistanceReward",
    "JG14 - NeedToChangeAttendantDialog", NULL
};
static const char *q_greece_jg16[] = {
    "JG16 - KnowofKnossosFog", "JG16 - TalkedToXanthippus", NULL
};

/* --- ACT II: Egypt --------------------------------------------------- */

static const char *q_egypt_scroll[] = {
    "Q08_PickedUpRhakotisScroll", NULL
};
static const char *q_egypt_ceremony[] = {
    "Q09_CeremonyDone", "Q09_FoundTempleofAtum", NULL
};
/* q_egypt_reward ("Q10_RewardGiven") removed — merged into q_egypt_telkine */
static const char *q_egypt_sickle[] = {
    "Q11_SickleRoomFound", NULL
};
static const char *q_egypt_telkine[] = {
    "Egypt - Telkine Defeated", NULL
};

/* Egypt side quests */
static const char *q_egypt_je01[] = {
    "JE01 - AssignedFindSword", NULL
};
static const char *q_egypt_je03[] = {
    "JE03 - KilledMonsters", NULL
};
static const char *q_egypt_je04[] = {
    "JE04 - AssignedReptillianRaiders", "JE04 - KilledMonsters", NULL
};
static const char *q_egypt_je05[] = {
    "JE05 - AssignedBeggarsHomes", "JE05 - KilledMonsters", NULL
};
static const char *q_egypt_je06[] = {
    "JE06 - AssignedHelpPriesthood", "JE06 - TalkedFirstTime",
    "JE06 - TalkedSecondTime", NULL
};
static const char *q_egypt_je07[] = {
    "JE07 - AssignedLostBrother", "JE07 - KilledMonster",
    "JE07 - KnowTheBrother", "JE07 - TalkedToSister", NULL
};
static const char *q_egypt_je09[] = {
    "JE09_TalkedtoScamp", NULL
};
static const char *q_egypt_je10[] = {
    "JE10 - AssignedAmbushedCaravan", "JE10 - KilledIznu",
    "JE10 - KnowOasisVillager", NULL
};
static const char *q_egypt_je11[] = {
    "JE11 - AssignedTransmogrifiedPriest", "JE11 - KilledPriest", NULL
};

/* --- ACT III: Orient ------------------------------------------------- */

static const char *q_orient_assign[] = {
    "Q12_ChangAnAssigned", "Q12_ChangAnWomanStepsForward",
    "Q12_FindAlalaAssigned", "Q12_SilkRoadAssigned", NULL
};
static const char *q_orient_telkine[] = {
    "Orient - Telkine Defeated", NULL
};

/* Orient side quests */
static const char *q_orient_jo00[] = {
    "JO00 - ToldAboutYeti", NULL
};
static const char *q_orient_jo01[] = {
    "JO1 - KilledTheMonsters", NULL
};
static const char *q_orient_jo03[] = {
    "JO3 - KilledIceSprites", "JO3 - TimeToPanic", NULL
};
static const char *q_orient_jo04[] = {
    "JO04 - KilledBoss", "JO04 - KilledSpirits",
    "JO04 - KnowFather", "JO04 - KnowSon", NULL
};
static const char *q_orient_jo06[] = {
    "JO6 - KilledMonsters", "JO6 - TalkedOnce", "JO6 - TalkedTwice", NULL
};
static const char *q_orient_jo07[] = {
    "JO7 - KilledTheRaptor", NULL
};
static const char *q_orient_jo08[] = {
    "JO08 - Assigned", "JO08 - KilledXiao", "JO08 - TalkedToGuardOnce", NULL
};
static const char *q_orient_jo10[] = {
    "JO10 - TalkedToCollector", NULL
};
static const char *q_orient_jo12[] = {
    "JO12 - BandariDefeated", "JO12 - EmperorRewardGiven", "JO12 - SetUp JO13", NULL
};
static const char *q_orient_jo13[] = {
    "JO13 - A Killed", "JO13 - B Killed", "JO13 - C Killed",
    "JO13 - D Killed", "JO13 - E Killed", NULL
};
static const char *q_orient_jo14[] = {
    "JO14 - AssignedClearShrine", "JO14 - KilledShrineMonsters", NULL
};
static const char *q_orient_jo16[] = {
    "JO16 - KnowSisterOne", "JO16 - LogUpdateThirdSister",
    "JO16 - SecondTalkToSisterThree", NULL
};

/* --- ACT IV: Immortal Throne ----------------------------------------- */

static const char *q_it_sentinels[] = {
    "xQ01_MedeaSentinelsKilled", "xQ01_QuestLogUpdated",
    "xQ01_StopLoopingTiresias", NULL
};
static const char *q_it_greys[] = {
    "xQ02_AllGreysKilled", "xQ02_EyeCeremonyCompleted",
    "xQ02_QuestLogUpdated", NULL
};
static const char *q_it_charon[] = {
    "xQ03_CharonKilled", "xQ03_QuestLogUpdated",
    "xQ03_StopLoopingTiresias", "xQ03_TiresiasCalledOut", NULL
};
static const char *q_it_elysium[] = {
    "xQ04_PortalToElysiumOpened", "xQ04_QuestLogUpdated", NULL
};
static const char *q_it_odysseus[] = {
    "xQ05_TalkedToOdysseus", NULL
};
static const char *q_it_hades[] = {
    "xQ06_FreedPersephone", "xQ06_HadesKilled_RemovePersephone",
    "xQ06_OneChainMonsterKilled", NULL
};

/* IT side quests */
static const char *q_it_sq01[] = {
    "xSQ01_CrabsKilled", "xSQ01_TalkedToHeadFisherman", NULL
};
static const char *q_it_sq02[] = {
    "xSQ02_AcolyteCanGiveElixir", "xSQ02_QuestComplete",
    "xSQ02_QuestLogUpdated", NULL
};
static const char *q_it_sq03[] = {
    "xSQ03_BeginTorchLighting", "xSQ03_EscortWorkerIsAlive",
    "xSQ03_TalkedToWorker", "xSQ03_TorchDone_GiveReward",
    "xSQ03_WaveASpawned", "xSQ03_WaveBSpawned", "xSQ03_WaveCSpawned", NULL
};
static const char *q_it_sq04[] = {
    "xSQ04_MinibossKilled", "xSQ04_OutpostFound", "xSQ04_OutpostSaved", NULL
};
static const char *q_it_sq05[] = {
    "xSQ05_PickedUpOneItem", "xSQ05_QuestAssigned", "xSQ05_ReadyForReward", NULL
};
static const char *q_it_sq06[] = {
    "xSQ06_TalkedToAcolyte", NULL
};
static const char *q_it_sq07[] = {
    "xSQ07_TalkedToHeadmistress", NULL
};
static const char *q_it_sq08[] = {
    "xSQ08_KilledSisterMonsters", "xSQ08_QuestCompleted",
    "xSQ08_TalkedToSister", NULL
};
static const char *q_it_sq09[] = {
    "xSQ09_AllNpcsSaved", "xSQ09_QuestAssigned", NULL
};
static const char *q_it_sq10[] = {
    "xSQ10_TalkedToDyingAdmetus", "xSQ10_TalkedToQuarterMaster", NULL
};
static const char *q_it_sq11[] = {
    "xSQ11_KilledMonsterCaptain", "xSQ11_TalkedToOldMan", NULL
};
static const char *q_it_sq12[] = {
    "xSQ12_LeaderMovedByBoat", "xSQ12_SavedLeaderShade",
    "xSQ12_TalkedToFrightenedShade", NULL
};
static const char *q_it_sq13[] = {
    "xSQ13_KilledStygianHydradon", "xSQ13_TalkedToObserver", NULL
};
static const char *q_it_sq14[] = {
    "xSQ14_TalkedToPriestess", NULL
};
static const char *q_it_sq15[] = {
    "xSQ15_OpenMirrorPortal", "xSQ15_OrpheusSaved",
    "xSQ15_TalkedToEurydice", NULL
};
static const char *q_it_sq16[] = {
    "xSQ16_AlreadyTalkedToDaemon", "xSQ16_KilledCaravans",
    "xSQ16_KilledFirstCaravan_SpawnMonsters", "xSQ16_QuestCompleted_ReadyFor xSQ17",
    "xSQ16_TalkedToRogue", NULL
};
static const char *q_it_sq17[] = {
    "xSQ17_CanTalkToBadGuyShade", "xSQ17_CanTalkToTurncoatDaemon",
    "xSQ17_SpawnAndTaskEnvoy", NULL
};
static const char *q_it_sq18[] = {
    "xSQ18_KilledNecromanteionMonster", "xSQ18_TalkedToEscapedPriest", NULL
};
static const char *q_it_sq19[] = {
    "xSQ19_FoundAKey", "xSQ19_TalkedToTreasureHunterOne",
    "xSQ19_UsedAStatue", "xSQ19_UsedFrostStatue", "xSQ19_UsedPyroStatue",
    "xSQ19_UsedSoulStatue", "xSQ19_UsedVenomStatue",
    "xSQ19_VaultDoorOpened", NULL
};
static const char *q_it_sq20[] = {
    "xSQ20_MoveAdmetusToElysium", "xSQ20_MoveAdmetusToToJ",
    "xSQ20_QuestAssigned", "xSQ20_SpawnAdmetusJailorInPoJ", NULL
};
static const char *q_it_sq21[] = {
    "xSQ21_EscortComplete_ReadyForNextQuest", "xSQ21_EscortMessengerIsAlive",
    "xSQ21_ReadyForEscortMessenger", NULL
};
static const char *q_it_sq22[] = {
    "xSQ22_BannerIsAlive", "xSQ22_StartQuest",
    "xSQ22_WaveOneKilled", "xSQ22_WaveTwoKilled",
    "xSQ22_WaveThreeKilled_MissionComplete", NULL
};
static const char *q_it_sq24[] = {
    "xSQ24_BloodWitchKilled", "xSQ24_SavedTroops", "xSQ24_TalkedToCaptain4", NULL
};
static const char *q_it_sq25[] = {
    "xSQ25_AllWalkersKilled", "xSQ25_QuestAssigned", NULL
};
static const char *q_it_sq26[] = {
    "xSQ26_OneCrystalKilled", "xSQ26_QuestAssigned", NULL
};
static const char *q_it_sq27[] = {
    "xSQ27_QuestAssigned", NULL
};

/* --- ACT V: Ragnarok ------------------------------------------------- */

static const char *q_rag_q01[] = {
    "Q01PorcusKilled", "Q01QuestComplete", "Q01RecievedQuestFromOligarch1", NULL
};
static const char *q_rag_q02[] = {
    "Q02BoatActivated", "Q02HeuneburgGatesOpen", "Q02_Assigned", NULL
};
static const char *q_rag_q03[] = {
    "Q03Elder1Killed", "Q03Elder2Killed", "Q03Elder3Killed", "Q03Elder4Killed",
    "Q03GlauberPrinceTalkedOnce", "Q03GlaubergGateOpen", "Q03GylfiHouseOpen",
    "Q03HasGoods1", "Q03HasGoods2", "Q03HasGoods3", "Q03NerthusRootDoorOpen", NULL
};
static const char *q_rag_q04[] = {
    "Q04GylfiSettlementGatesOpen", "Q04QuestComplete", "Q04SigtunaExplored",
    "Q04_GorgonsDefeated", NULL
};
static const char *q_rag_q05[] = {
    "Q05BaldrDead", "Q05BoatActivated", "Q05ChieftessUpdateGiven",
    "Q05DvergrHallsOpen", "Q05FakeYlvaRescued", "Q05FungoidCavernOpen",
    "Q05FungoidKingDead", "Q05HasIchthianRelic", "Q05MFCOpen",
    "Q05NidbaldActivated", "Q05OdinHasSpoken1", "Q05OdinHasSpoken2",
    "Q05SecretPassageOpen", "Q05TalkedToLoki", "Q05UpgradeQuestUnlocked",
    "Q05YlvaRunACompleted", "Q05YlvaRunBCompleted", "Q05YlvaRunCCompleted",
    "Q05YlvaRunComplete", "Q05YlvaRunDCompleted", "Q05YlvaRunECompleted",
    "Q05YlvaRunFCompleted", "YlvaQuest_YlvaRescued", NULL
};
static const char *q_rag_q06[] = {
    "Q06BifrostActivated", "Q06BifrostSabotaged",
    "Q06OdinRunComplete", "Q06TalkedToOdin", NULL
};
static const char *q_rag_q07[] = {
    "Q07FireWallUnlocked", "Q07MimerDead", "Q07SurtrDead", "Q07TalkedtoYlva",
    "Q7LokiRevealed", "Q7MimerInterrogated", NULL
};
static const char *q_rag_epilogue[] = {
    "EPILOGUEOdinTalkedOnce", NULL
};

/* Ragnarok side quests (SQ12-SQ22) */
static const char *q_rag_sq12[] = {
    "SQ12AttackerKilled", "SQ12Completed", "SQ12DialogueA1Done",
    "SQ12DialogueA2Done", "SQ12EscortStarted", "SQ12NixieArrived",
    "SQ12NixieSurvived", NULL
};
static const char *q_rag_sq14[] = {
    "SQ14Completed", NULL
};
static const char *q_rag_sq15[] = {
    "SQ15Completed", NULL
};
static const char *q_rag_sq16[] = {
    "SQ16Completed", NULL
};
static const char *q_rag_sq17[] = {
    "SQ17Completed", "SQ17Completed_Alive", "SQ17ExclamationDone",
    "SQ17GotTeeth", "SQ17QuestOngoing", NULL
};
static const char *q_rag_sq19[] = {
    "SQ19Completed", "SQ19Completed_Armor", "SQ19ExclamationDone", "SQ19GotQuest", NULL
};
static const char *q_rag_sq20[] = {
    "SQ20Completed", "SQ20ExclamationDone", "SQ20GotNecklace", NULL
};
static const char *q_rag_sq21[] = {
    "SQ21Completed", "SQ21ReturnedTabletAB", NULL
};
static const char *q_rag_sq22[] = {
    "SQ22GotQuest", "SQ22_Completed", "SQ22_DurinUsed", NULL
};

/* --- ACT VI: Atlantis ------------------------------------------------ */

static const char *q_atl_main[] = {
    "MQFirstTalkMarinosDone", "MQThirdTalkMarinosDone",
    "MQTalkAcrocatusDone", "MQTalkAcrocatus2Done",
    "MQTalkAcrocatusTempleDone", "MQAcrocatusFinalTalk",
    "MQAcrocatusReadyToBeKilled",
    "MQTalkHighPriestDone", "MQFoundLostDiary",
    "MQFirstLeaderKilled", "MQSecondLeaderKilled", "MQThirdLeaderKilled",
    "MQTelkineDead", NULL
};

/* Atlantis side quests — tokens use bare SQ## prefix (no X3_ prefix).
 * X3_SQ##GotQuest tokens are "got quest" triggers; SQ##Complete = completion. */
static const char *q_atl_new_adventures[] = {
    "X3_GotQuest_FindMarinos", "X3_FindMarinos_Complete", NULL
};
static const char *q_atl_sq01[] = {
    "SQ01Complete", "SQ01Completed", "SQ01ExclamationDone",
    "SQ01GotQuest", "SQ01HasKilledEnemies", "SQ01WandererSaved",
    "X3_SQ01GotQuest", NULL
};
static const char *q_atl_sq02[] = {
    "SQ02Complete", "SQ02Completed", "SQ02HookahRetrieved", "SQ02TalkedToJibran", NULL
};
static const char *q_atl_sq03[] = {
    "SQ03Complete", "SQ03Completed", "SQ03FoundTreveros", "SQ03GotQuest",
    "SQ03GotQuestFromPhaedrus", "SQ03GotQuestFromTreveros", "SQ03HasGoat", NULL
};
static const char *q_atl_sq04[] = {
    "SQ04Complete", "SQ04GotQuest", "SQ04QuestCompleted", "SQ04QuestCompleted_Mineowner",
    "X3_SQ04GotQuest", NULL
};
static const char *q_atl_sq05[] = {
    "SQ05Complete", "SQ05DoorOpened", "SQ05ExclamationDone", "SQ05GotPassword",
    "SQ05GotQuest", "SQ05KilledBasilisks", "SQ05TalkedToWife", NULL
};
static const char *q_atl_sq06[] = {
    "SQ06Complete", "SQ06Completed", "SQ06FoundFruit", "SQ06GotQuest",
    "X3_SQ06GotQuest", NULL
};
static const char *q_atl_sq07[] = {
    "SQ07Complete", "SQ07FoundWine", "SQ07GotQuest",
    "SQ07ShortQuestComplete", "SQ07TalkedToCook", NULL
};
static const char *q_atl_sq08[] = {
    "SQ08Complete", "SQ08Completed", "SQ08KilledMonsterA", "SQ08KilledMonsterB", NULL
};
static const char *q_atl_sq09[] = {
    "SQ09Complete", "SQ09Completed", "SQ09GotQuestFromJudoc", "SQ09KilledHighPriest", NULL
};
static const char *q_atl_sq10[] = {
    "SQ10Complete", "SQ10Completed", "SQ10GotQuest", NULL
};
static const char *q_atl_sq11[] = {
    "SQ11Complete", "SQ11Completed", "SQ11Completed_Kaupangr",
    "SQ11GotQuest", "SQ11KilledLeaderA", "SQ11KilledLeaderB", NULL
};

/* Shared empty token array for quests with no known tokens in QuestToken.myw */
static const char *q_no_tokens[] = { NULL };

/* Orient: Olympus main quest has one orphaned token */
static const char *q_orient_olympus[] = {
    "Olympus - Typhon Defeated", NULL
};

/* --- ACT VII: Eternal Embers ----------------------------------------- */

static const char *q_ee_jc001[] = {
    "x4MQ_JC001_Immortal_Throne_Started", "x4MQ_JC001_Talked_With_ZhangWei",
    "x4MQ_JC001_EmperorYao_Conversation1", "x4MQ_JC001_EmperorYao_Conversation_Finished",
    "x4MQ_JC001_TalkedWithVanguard", "x4MQ_JC001_MaFang_Met",
    "x4MQ_JC001_HouYi_Met", "x4MQ_JC001_PrayerBeads_Received",
    "x4MQ_JC001_TigerBreakWallStarted", "x4MQ_JC001_Wave1_SpawnedOnce",
    "x4MQ_JC001_Monkeys_Killed",
    "x4MQ_JC001_Bow_Piece_A_Collected", "x4MQ_JC001_Bow_Piece_B_Collected",
    "x4MQ_JC001_BothBowPiecesCollected", "x4MQ_JC001_Bow_Pieces_Returned",
    "x4MQ_JC001_FakeQiongQi_Killed", "x4MQ_JC001_FakeQiongQi_ArenaExitable",
    "x4MQ_JC001_OpenedFakeQiongQi_WithTalking",
    "x4MQ_JC001_QiongQi_First_Spawned", "x4MQ_JC001_QiongQi_Ran_Away",
    "x4MQ_JC001_Real_QiongQi_Spawned", "x4MQ_JC001_QiongQi_Real_Killed",
    "x4MQ_JC001_CHEST_Real_QiongQi_Killed",
    "x4MQ_JC001_1st_Talk_With_Wounded_Sailor",
    "x4MQ_JC001_Mausoleum_CrystalKey", "x4MQ_JC001_Mausoleum_GoldKey",
    "x4MQ_JC001_Mausoleum_JadeKey", "x4MQ_JC001_Mausoleum_SilverKey",
    "x4MQ_JC001_Completed", "x4_GDS_MQ_JC001_Completed", NULL
};
static const char *q_ee_jc002[] = {
    "x4MQ_JC002_First_Talk_LiuChangDong",
    "x4MQ_JC002_MausoleumEntered", "x4MQ_JC002_MausoleumKeyReceived",
    "x4MQ_JC002_GotAllMausoleumKeys",
    "x4MQ_JC002_CrystalKeyUsed", "x4MQ_JC002_CrystalKey_Received",
    "x4MQ_JC002_GoldKeyUsed", "x4MQ_JC002_JadeKeyUsed", "x4MQ_JC002_SilverKeyUsed",
    "x4MQ_JC002_MausoleumExitOpened", "x4MQ_JC002_LeftMausoleum",
    "x4MQ_JC002_KilledTerracottaElite", "x4MQJC002_BossChest_TerracottaElite",
    "x4MQ_JC002_HouYi_Callout_Done",
    "x4MQ_JC002_SihaiLongwangFightStart", "x4MQ_JC002_SihaiLongwang_Phase1_FirstTime",
    "x4MQ_JC002_SihaiLongwangKilled", "x4MQ_JC002_CHEST_SihaiLongwang_Killed",
    "x4MQ_JC002_SihaiLonwang_PostFightTalk",
    "x4MQ_JC002_Talked_With_Jade_Emperor",
    "x4MQ_JC002_Completed", "x4_GDS_MQ_JC002_Completed", NULL
};
static const char *q_ee_jc003[] = {
    "x4MQ_JC003_EmperorFirstTalk", "x4MQ_JC003_MarshlandPermission",
    "x4MQ_JC003_MarshlandUnlocked", "x4MQ_JC003_SunsSpawned",
    "x4MQ_JC003_SunsSpawned_Once", "x4MQ_JC003_FinalSunKilled_RESETTING",
    "x4MQ_JC003_Suns_MergeSequence_GameReloaded",
    "x4MQ_JC003_RepeatCutscene", "x4MQ_JC003_OrbReturned",
    "x4MQ_JC003_CHEST_Suns_Killed",
    "x4MQ_JC003_Completed", "x4_GDS_MQ_JC003_Completed", NULL
};
static const char *q_ee_je001[] = {
    "x4MQ_JE001_Imhotep_FirstConversation", "x4MQ_JE001_ImhotepConversation_Finished",
    "x4MQ_JE001_Tut_First_Conversation", "x4MQ_JE001_Tut_Finished_Conversation",
    "x4MQ_JE001_TutankhamunSecretReward",
    "x4MQ_JE001_Fallback",
    "x4MQ_JE001_Akenathen_PillarStart", "x4MQ_JE001_Akhenaten_CALLOUT_Start",
    "x4MQ_JE001_AkhenathenConversationDone", "x4MQ_JE001_AkhenathenKilled",
    "x4MQ_JE001_CHEST_Akenathen_Killed",
    "x4MQ_JE001_Zazamankh_Callout", "x4MQ_JE001_Zazamankh_Callout_Finished",
    "x4MQ_JE001_Zazamankh_TurnedEnemy", "x4MQ_JE001_Zazamank_Killed",
    "x4MQ_JE001_Zazamankh_PostFight_Conversation",
    "x4MQ_JE001_CHEST_Zazamankh_Killed",
    "x4MQ_JE001_AbydosExitOpened", "x4MQ_JE001_PortalToSihai_Opened",
    "x4MQ_JE001_Completed", "x4_GDS_MQ_JE001_Completed", NULL
};

/* EE side quests */
static const char *q_ee_sq101[] = {
    "x4SQ101_FoundFloodedCave", "x4SQ101_FoundHauntedTree",
    "x4SQ101_PirateCaptain_Transformation_Started",
    "x4SQ101_Pirate_Captain_Turned_Enemy",
    "x4SQ101_Killed_Ghost_Pirate_HERO",
    "x4SQ101_Completed", "x4_GDS_SQ_101_Completed", NULL
};
static const char *q_ee_sq102[] = {
    "x4SQ102_Assigned", "x4SQ102_Callout", "x4SQ102_MonkTempleFound",
    "x4SQ102_GotHerb", "x4SQ102_HerbIlluminateReset",
    "x4SQ102_Fighting_FaHai", "x4SQ102_BossChest_FaHai",
    "x4SQ102_Completed", "x4SQ102_Completed_GameReloaded",
    "x4_GDS_SQ_102_Completed", NULL
};
static const char *q_ee_sq104[] = {
    "x4SQ104_QuestAssigned", "x4SQ104_WentInsideSlavePits",
    "x4SQ104_MustLightUpShrine",
    "x4SQ104_AuthorizedToSaveA", "x4SQ104_AuthorizedToSaveB",
    "x4SQ104_AuthorizedToSaveC", "x4SQ104_AuthorizedToSaveD",
    "x4SQ104_AuthorizedToSaveE",
    "x4SQ104_SavedSlaveA", "x4SQ104_SavedSlaveB",
    "x4SQ104_SavedSlaveC", "x4SQ104_SavedSlaveD",
    "x4SQ104_SavedSlaveE", "x4SQ104_Slave_A_Last",
    "x4SQ104_RescuedAllPrisoners", "x4SQ104_BossChest_Sanshou",
    "x4SQ104_Completed", "x4_GDS_SQ_104_Completed", NULL
};
static const char *q_ee_sq105[] = {
    "x4SQ105_QuestAssigned", "x4SQ105_PickedUpIdol",
    "x4SQ105_Gorilla_Killed", "x4SQ105_BossChest_GorillaShaman",
    "x4SQ105_QuestCompleted", "x4_GDS_SQ_105_Completed", NULL
};
static const char *q_ee_sq201[] = {
    "x4SQ201_QuestAssigned", "x4SQ201_Blueprintfound",
    "x4SQ201_KwanYu_Callout", "x4SQ201_ProgressAnimationStart",
    "x4SQ201_RandomizerFired", "x4SQ201_RandomizerVersionA",
    "x4SQ201_RepairCallout_1", "x4SQ201_RepairCallout_2",
    "x4SQ201_RepairCallout_3", "x4SQ201_RingBell",
    "x4SQ201_QuestCompleted", "x4_GDS_SQ_201_Completed", NULL
};
static const char *q_ee_sq202[] = {
    "x4SQ202_Quest_Assigned", "x4SQ202_HasTalkedtoGroom",
    "x4SQ202_FirstItemGiven", "x4SQ202_KilledProxy",
    "x4SQ202_LiMin_Callout_Finished", "x4SQ202_BossChest_LiQiang",
    "x4SQ202_Quest_Finished", "x4_GDS_SQ_202_Completed", NULL
};
static const char *q_ee_sq203[] = {
    "x4SQ203_MogwaisEntranceDead", "x4SQ203_MingyuRescued",
    "x4SQ203_MogwaisTaskmasterDead", "x4SQ203_BossChest_MogwaiTaskmaster",
    "x4SQ203_QuestCompleted", "x4_GDS_SQ_203_Completed", NULL
};
static const char *q_ee_sq204[] = {
    "x4SQ204_QuestAssigned", "x4SQ204_EnterVolume",
    "x4SQ204_HuoshenDefeated", "x4SQ204_BossChest_Huoshen",
    "x4SQ204_QuestCompleted", "x4_GDS_SQ_204_Completed", NULL
};
static const char *q_ee_sq205[] = {
    "x4SQ205_TalkedWithZhinu", "x4SQ205_ZhinuCallout",
    "x4SQ205_SunsChampion_Spawned", "x4SQ205_SunsChampion_Killed",
    "x4SQ205_Quest_Completed", "x4_GDS_SQ_205_Completed", NULL
};
static const char *q_ee_sq206[] = {
    "x4SQ206_RandomizerVersionA1", "x4SQ206_RandomizerVersionC2",
    "x4SQ206_StartCombat",
    "x4SQ206_QuestFinished", "x4_GDS_SQ_206_Completed", NULL
};
static const char *q_ee_sq301[] = {
    "x4SQ301_QuestAssigned", "x4SQ301_ShouldPossessStone",
    "x4SQ301_ExhumedMedjai_Killed",
    "x4SQ301_QuestReturned", "x4_GDS_SQ_301_Completed", NULL
};
static const char *q_ee_sq302[] = {
    "x4SQ302_QuestAssigned", "x4SQ302_FoundScorpionCave",
    "x4SQ302_Scorpion_A_Killed", "x4SQ302_Scorpion_B_Killed",
    "x4SQ302_Scorpion_C_Killed", "x4SQ302_HasThreeVials",
    "x4SQ302_QuestFinished", "x4_GDS_SQ_302_Completed", NULL
};
static const char *q_ee_sq303[] = {
    "x4SQ303_QuestAssigned", "x4SQ303_LootersFound",
    "x4SQ303_Looters_Set1_Defeated", "x4SQ303_Looters_Set2_Defeated",
    "x4SQ303_Looters_Set3_Defeated", "x4SQ303_LootersDefeated",
    "x4SQ303_BossChest_LooterLeader",
    "x4SQ303_QuestFinished", "x4_GDS_SQ_303_Completed", NULL
};
static const char *q_ee_sq401[] = {
    "x4SQ401_QuestAssigned", "x4SQ401_Jinchan_Callout_Started",
    "x4SQ401_Jinchan_Callout_Finished",
    "x4SQ401_GuardiansKilled", "x4SQ401_FenghuangRescued",
    "x4SQ401_Quest_Completed", "x4_GDS_SQ_401_Completed", NULL
};
static const char *q_ee_sq402[] = {
    "x4SQ402_QuestAssigned", "x4SQ402_DoorOpened",
    "x4SQ402_DemonKilled",
    "x4SQ402_QuestFinished", "x4_GDS_SQ_402_Completed", NULL
};
static const char *q_ee_ss001[] = {
    "x4SS001_Idol_Returned", "x4SS001_QiongQi_SummerResidence_Done", NULL
};

/* ── Master quest definitions table ───────────────────────────────────── */

static const QuestDef quest_defs[] = {
    /* === ACT I: Greece === */
    /* Main quests — 8 quests per testdata/quests.txt */
    { "A Troubled Village",          NULL, ACT_GREECE, true,  q_greece_horse,    "Q1_HorsePartCompleted" },
    { "Spartans at War",             NULL, ACT_GREECE, true,  q_greece_shaman,   "Q1_ShamanPartCompleted" },
    { "The Words of the Oracle",     NULL, ACT_GREECE, true,  q_greece_nessus,   "Q2_NessusKilled" },
    { "The Source of the Monsters",  NULL, ACT_GREECE, true,  q_no_tokens,       "Q2_NessusKilled" },
    { "The Battle for Athens",       NULL, ACT_GREECE, true,  q_no_tokens,       "Q2_NessusKilled" },
    { "The Order of Prometheus",     NULL, ACT_GREECE, true,  q_no_tokens,       "Q2_NessusKilled" },
    { "Under the Labyrinth",        NULL, ACT_GREECE, true,  q_no_tokens,       "Q2_NessusKilled" },
    { "The Blindness of the Gods",   NULL, ACT_GREECE, true,  q_greece_telkine,  "Greece - Telkine Defeated" },
    /* Side quests — ordered to match in-game quest log (testdata/quests.txt) */
    { "Monstrous Brigands",          "Helos",   ACT_GREECE, false, q_greece_jg02,    "JG02 - KilledMonsters" },
    { "The Cornered Man",            "Helos",   ACT_GREECE, false, q_greece_jg03,    "JG03 - KilledBrute" },
    { "Medicines Waylaid",           "Helos",   ACT_GREECE, false, q_greece_jg01,    "JG01 - AssignedShepherdTrapped" },
    { "The Lost Dowry",              "Sparta",  ACT_GREECE, false, q_greece_jg04,    "JG04 - AssignedFindDowry" },
    { "The Ancient of War",          "Sparta",  ACT_GREECE, false, q_greece_jg05,    "JG05 - FoundOldSoldier" },
    { "The Poisoned Spring",         "Sparta",  ACT_GREECE, false, q_greece_jg06,    "JG06 - KilledSpiders" },
    { "Skeleton Raiders",            "Megara",  ACT_GREECE, false, q_greece_jg07,    "JG07 - KilledThreeBrothers" },
    { "News of a Shipwreck",         "Megara",  ACT_GREECE, false, q_greece_jg09,    "JG09 - AssignedFindShipwreck" },
    { "A Proper Offering",           "Delphi",  ACT_GREECE, false, q_no_tokens,      "JG10 - TalkedToChiron" },
    { "The Good Centaur",            "Delphi",  ACT_GREECE, false, q_greece_jg10,    "JG10 - TalkedToChiron" },
    { "A Master Blacksmith",         "Delphi",  ACT_GREECE, false, q_greece_jg11,    "JG11 - KilledFlamingBoarMan" },
    { "Goods Abandoned",             "Delphi",  ACT_GREECE, false, q_no_tokens,      "JG11 - KilledFlamingBoarMan" },
    { "The Grieving Widow",          "Delphi",  ACT_GREECE, false, q_greece_jg12,    "JG12 - KilledGiantLimos" },
    { "Trapped in the Ruins",        "Athens",  ACT_GREECE, false, q_greece_jg13,    "JG13 - FoundLostScout" },
    { "Spartans Lost",               "Athens",  ACT_GREECE, false, q_greece_jg14,    "JG14 - ArachneKilled" },
    { "Xanthippus the Healer",       "Knossos", ACT_GREECE, false, q_greece_jg16,    "JG16 - TalkedToXanthippus" },
    { "The Undead Tyrant",           "Knossos", ACT_GREECE, false, q_no_tokens,      "JG16 - TalkedToXanthippus" },

    /* === ACT II: Egypt === */
    /* Main quests — 4 quests per testdata/quests.txt */
    { "The Blindness of the Gods",   NULL, ACT_EGYPT, true,  q_egypt_scroll,     "Q08_PickedUpRhakotisScroll" },
    { "The Invocation",              NULL, ACT_EGYPT, true,  q_egypt_ceremony,   "Q09_CeremonyDone" },
    { "A Telkine in Egypt",          NULL, ACT_EGYPT, true,  q_egypt_telkine,    "Egypt - Telkine Defeated" },
    { "The Sickle of Kronos",        NULL, ACT_EGYPT, true,  q_egypt_sickle,     "Q11_SickleRoomFound" },
    /* Side quests — ordered to match in-game quest log (testdata/quests.txt) */
    { "The Family Heirloom",         "Rhakotis",      ACT_EGYPT, false, q_egypt_je01,       "JE01 - AssignedFindSword" },
    { "The Beast of Legend",         "Lower Nile",    ACT_EGYPT, false, q_egypt_je03,       "JE03 - KilledMonsters" },
    { "Plight of the Nile Farmers",  "Lower Nile",    ACT_EGYPT, false, q_egypt_je05,       "JE05 - KilledMonsters" },
    { "A Promethean Surrounded",     "Lower Nile",    ACT_EGYPT, false, q_egypt_je04,       "JE04 - KilledMonsters" },
    { "Lowest of the Low",           "Memphis",       ACT_EGYPT, false, q_egypt_je06,       "JE06 - TalkedSecondTime" },
    { "The High Priest's Request",   "Memphis",       ACT_EGYPT, false, q_no_tokens,        "JE06 - TalkedSecondTime" },
    { "The Missing Brother",         "Memphis",       ACT_EGYPT, false, q_egypt_je07,       "JE07 - KilledMonster" },
    { "Khufu's Curse",               "Giza",          ACT_EGYPT, false, q_no_tokens,        "JE07 - KilledMonster" },
    { "A Hidden Treasure",           "Fayum Oasis",   ACT_EGYPT, false, q_egypt_je09,       "JE09_TalkedtoScamp" },
    { "Caravan Woes",                "Fayum Oasis",   ACT_EGYPT, false, q_egypt_je10,       "JE10 - KilledIznu" },
    { "The Corrupted Priest",        "Thebes",        ACT_EGYPT, false, q_egypt_je11,       "JE11 - KilledPriest" },

    /* === ACT III: Orient === */
    /* Main quests — 5 quests per testdata/quests.txt */
    { "The Sickle of Kronos",        NULL, ACT_ORIENT, true,  q_no_tokens,        "Q12_SilkRoadAssigned" },
    { "Hunt for the Sickle",         NULL, ACT_ORIENT, true,  q_orient_assign,    "Q12_SilkRoadAssigned" },
    { "Journey to the Jade Palace",  NULL, ACT_ORIENT, true,  q_no_tokens,        "Orient - Telkine Defeated" },
    { "Under Wusao Mountain",        NULL, ACT_ORIENT, true,  q_orient_telkine,   "Orient - Telkine Defeated" },
    { "Olympus",                     NULL, ACT_ORIENT, true,  q_orient_olympus,   "Olympus - Typhon Defeated" },
    /* Side quests — ordered to match in-game quest log (testdata/quests.txt) */
    { "The Seeds of Destruction",    "Babylon",    ACT_ORIENT, false, q_orient_jo01,      "JO1 - KilledTheMonsters" },
    { "A Gargantuan Yeti",           "Silk Road",  ACT_ORIENT, false, q_orient_jo00,      "JO00 - ToldAboutYeti" },
    { "Mystery in the Mountains",    "Silk Road",  ACT_ORIENT, false, q_orient_jo03,      "JO3 - KilledIceSprites" },
    { "Caravan in Trouble",          "Silk Road",  ACT_ORIENT, false, q_orient_jo04,      "JO04 - KilledBoss" },
    { "The Child and the Raptor",    "Great Wall", ACT_ORIENT, false, q_orient_jo07,      "JO7 - KilledTheRaptor" },
    { "Peng Problems",               "Great Wall", ACT_ORIENT, false, q_orient_jo08,      "JO08 - KilledXiao" },
    { "Stalker in the Woods",        "Great Wall", ACT_ORIENT, false, q_no_tokens,        "JO08 - KilledXiao" },
    { "The Wealthy Collector",       "Great Wall", ACT_ORIENT, false, q_orient_jo10,      "JO10 - TalkedToCollector" },
    { "A Lesson in Despair",         "Great Wall", ACT_ORIENT, false, q_orient_jo06,      "JO6 - KilledMonsters" },
    { "The Emperor's Clay Soldiers", "Chang'an",   ACT_ORIENT, false, q_orient_jo12,      "JO12 - BandariDefeated" },
    { "Terra Cottas At Large",       "Chang'an",   ACT_ORIENT, false, q_orient_jo13,      "JO13 - E Killed" },
    { "Behind the Waterfall",        "Chang'an",   ACT_ORIENT, false, q_orient_jo14,      "JO14 - KilledShrineMonsters" },
    { "A General in Repose",         "Oiyum",      ACT_ORIENT, false, q_no_tokens,        "JO16 - SecondTalkToSisterThree" },
    { "The Hermit Mage",             "Oiyum",      ACT_ORIENT, false, q_no_tokens,        "JO16 - SecondTalkToSisterThree" },
    { "Three Sisters",               "Oiyum",      ACT_ORIENT, false, q_orient_jo16,      "JO16 - SecondTalkToSisterThree" },

    /* === ACT IV: Immortal Throne === */
    /* Main quests (xtagxQ##_Title) */
    { "A Mysterious Message",        NULL, ACT_IMMORTAL_THRONE, true,  q_it_sentinels,      "xQ01_MedeaSentinelsKilled" },
    { "Medea's Price",               NULL, ACT_IMMORTAL_THRONE, true,  q_it_greys,          "xQ02_AllGreysKilled" },
    { "The Road to Hades",           NULL, ACT_IMMORTAL_THRONE, true,  q_it_charon,         "xQ03_CharonKilled" },
    { "Judgment of the Living",      NULL, ACT_IMMORTAL_THRONE, true,  q_it_elysium,        "xQ04_PortalToElysiumOpened" },
    { "The Battle for Elysium",      NULL, ACT_IMMORTAL_THRONE, true,  q_it_odysseus,       "xQ05_TalkedToOdysseus" },
    { "The Immortal Throne",         NULL, ACT_IMMORTAL_THRONE, true,  q_it_hades,          "xQ06_HadesKilled_RemovePersephone" },
    /* Side quests (xtagxSQ##_Title) */
    { "A Crab Story",                "Rhodes",              ACT_IMMORTAL_THRONE, false, q_it_sq01, "xSQ01_CrabsKilled" },
    { "An Impossible Task",          "Rhodes",              ACT_IMMORTAL_THRONE, false, q_it_sq02, "xSQ02_QuestComplete" },
    { "The Torch-Lighter's Gauntlet","Rhodes",              ACT_IMMORTAL_THRONE, false, q_it_sq03, "xSQ03_TorchDone_GiveReward" },
    { "Outpost in the Woods",        "Rhodes",              ACT_IMMORTAL_THRONE, false, q_it_sq04, "xSQ04_OutpostSaved" },
    { "The Stolen Sigil",            "Ixian Woods",         ACT_IMMORTAL_THRONE, false, q_it_sq05, "xSQ05_ReadyForReward" },
    { "The Wealth of Ancient Kings", "Ixian Woods",         ACT_IMMORTAL_THRONE, false, q_it_sq06, "xSQ06_TalkedToAcolyte" },
    { "Lampido's Potion",            "Ixian Woods",         ACT_IMMORTAL_THRONE, false, q_it_sq07, "xSQ07_TalkedToHeadmistress" },
    { "The Treasure Hunters",        "Ixian Woods",         ACT_IMMORTAL_THRONE, false, q_it_sq08, "xSQ08_QuestCompleted" },
    { "Among the Ruins",             "Epirus",              ACT_IMMORTAL_THRONE, false, q_it_sq09, "xSQ09_AllNpcsSaved" },
    { "A Dangerous Mission",         "Epirus",              ACT_IMMORTAL_THRONE, false, q_it_sq10, "xSQ10_TalkedToDyingAdmetus" },
    { "The Enemy's Captain",         "Epirus",              ACT_IMMORTAL_THRONE, false, q_it_sq11, "xSQ11_KilledMonsterCaptain" },
    { "The Stygian Lurker",          "Styx",                ACT_IMMORTAL_THRONE, false, q_it_sq13, "xSQ13_KilledStygianHydradon" },
    { "One Who Would Lead",          "Styx",                ACT_IMMORTAL_THRONE, false, q_it_sq12, "xSQ12_SavedLeaderShade" },
    { "Hades' Treasury",             "Plains of Judgement", ACT_IMMORTAL_THRONE, false, q_it_sq14, "xSQ14_TalkedToPriestess" },
    { "The Dust of a Titan",         "Plains of Judgement", ACT_IMMORTAL_THRONE, false, q_it_sq19, "xSQ19_VaultDoorOpened" },
    { "Eurydice and Orpheus",        "Plains of Judgement", ACT_IMMORTAL_THRONE, false, q_it_sq15, "xSQ15_OrpheusSaved" },
    { "An Invitation",               "Plains of Judgement", ACT_IMMORTAL_THRONE, false, q_it_sq16, "xSQ16_QuestCompleted_ReadyFor xSQ17" },
    { "The Necromanteion",           "Plains of Judgement", ACT_IMMORTAL_THRONE, false, q_it_sq18, "xSQ18_KilledNecromanteionMonster" },
    { "Admetus Among the Dead",      "Plains of Judgement", ACT_IMMORTAL_THRONE, false, q_it_sq20, "xSQ20_MoveAdmetusToElysium" },
    { "An Inside Source",            "Plains of Judgement", ACT_IMMORTAL_THRONE, false, q_it_sq17, "xSQ17_SpawnAndTaskEnvoy" },
    { "The Siege Striders",          "Elysium",             ACT_IMMORTAL_THRONE, false, q_it_sq24, "xSQ24_BloodWitchKilled" },
    { "Flight of the Messenger",     "Elysium",             ACT_IMMORTAL_THRONE, false, q_it_sq21, "xSQ21_EscortComplete_ReadyForNextQuest" },
    { "The Achaean Pass",            "Elysium",             ACT_IMMORTAL_THRONE, false, q_it_sq25, "xSQ25_AllWalkersKilled" },
    { "A Noisy Diversion",           "Elysium",             ACT_IMMORTAL_THRONE, false, q_it_sq22, "xSQ22_WaveThreeKilled_MissionComplete" },
    { "The Shards of Erebus",        "Palace of Hades",     ACT_IMMORTAL_THRONE, false, q_it_sq26, "xSQ26_OneCrystalKilled" },
    { "Hades' Generals",             "Palace of Hades",     ACT_IMMORTAL_THRONE, false, q_it_sq27, "xSQ27_QuestAssigned" },

    /* === ACT V: Ragnarok === */
    /* Main quests (x2tagx2Q##_Title) */
    { "Troubles of a New Age",       NULL, ACT_RAGNAROK, true,  q_rag_q01,       "Q01QuestComplete" },
    { "The Warrior Princess",        NULL, ACT_RAGNAROK, true,  q_rag_q02,       "Q02BoatActivated" },
    { "The Power of Nerthus",        NULL, ACT_RAGNAROK, true,  q_rag_q03,       "Q03NerthusRootDoorOpen" },
    { "Scandia under Siege",         NULL, ACT_RAGNAROK, true,  q_rag_q04,       "Q04QuestComplete" },
    { "The Rescue",                  NULL, ACT_RAGNAROK, true,  q_rag_q05,       "Q05YlvaRunComplete" },
    { "The Wisest Being",            NULL, ACT_RAGNAROK, true,  q_rag_q06,       "Q06OdinRunComplete" },
    { "The Burning Sword",           NULL, ACT_RAGNAROK, true,  q_rag_q07,       "Q07SurtrDead" },
    { "Epilogue: Odin",              NULL, ACT_RAGNAROK, true,  q_rag_epilogue,  "EPILOGUEOdinTalkedOnce" },
    /* Side quests — ordered to match in-game quest log (testdata/quests.txt) */
    { "Festivities",                 "Corinth",    ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "Sciron",                      "Corinth",    ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "A Northern Contact",          "Corinth",    ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "Heart to Stomach",            "Heuneburg",  ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "White Gold",                  "Heuneburg",  ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "The Golden Sickle",           "Heuneburg",  ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "Wine from the Rhine",         "Glauberg",   ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "The Troubled Son",            "Glauberg",   ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "Celtic Plaid",                "Glauberg",   ACT_RAGNAROK, false, q_no_tokens, "Q01QuestComplete" },
    { "Little Friends",              "Wildlands",  ACT_RAGNAROK, false, q_no_tokens, "SQ12Completed" },
    { "The Trapped Nixie",           "Wildlands",  ACT_RAGNAROK, false, q_rag_sq12, "SQ12Completed" },
    { "The Kornwyt's Scythe",        "Wildlands",  ACT_RAGNAROK, false, q_no_tokens, "SQ12Completed" },
    { "Giesel",                      "Scandia",    ACT_RAGNAROK, false, q_rag_sq17, "SQ17Completed" },
    { "Fir Cone Liquor",             "Scandia",    ACT_RAGNAROK, false, q_rag_sq19, "SQ19Completed" },
    { "The Restless King",           "Scandia",    ACT_RAGNAROK, false, q_rag_sq15, "SQ15Completed" },
    { "The Magic Cauldron",          "Scandia",    ACT_RAGNAROK, false, q_rag_sq14, "SQ14Completed" },
    { "The Survivor",                "Scandia",    ACT_RAGNAROK, false, q_rag_sq16, "SQ16Completed" },
    { "Dvergar History",             "Dark Lands", ACT_RAGNAROK, false, q_rag_sq20, "SQ20Completed" },
    { "Squabbling Merchants",        "Dark Lands", ACT_RAGNAROK, false, q_rag_sq21, "SQ21Completed" },
    { "The Craftsman's Passion",     "Dark Lands", ACT_RAGNAROK, false, q_no_tokens, "SQ22_Completed" },
    { "Legendary Craftsmanship",     "Dark Lands", ACT_RAGNAROK, false, q_rag_sq22, "SQ22_Completed" },

    /* === ACT VI: Atlantis === */
    /* All quests are side quests — Atlantis has no main quest tab in-game.
     * Tokens use bare SQ## prefix; completion = SQ##Complete. */
    { "New Adventures Await",        "Portal to Gadir",    ACT_ATLANTIS, false, q_atl_new_adventures, "X3_FindMarinos_Complete" },
    { "The City of Atlas",           "Atlantis",           ACT_ATLANTIS, false, q_atl_main,  "MQTelkineDead" },
    { "The Mysterious Artifacts",    "Atlantis",           ACT_ATLANTIS, false, q_atl_sq02, "SQ02Complete" },
    { "The Exterminator",            "Atlantis",           ACT_ATLANTIS, false, q_atl_sq05, "SQ05Complete" },
    { "Ancient Craft",               "Atlantis",           ACT_ATLANTIS, false, q_atl_sq09, "SQ09Complete" },
    { "A Score to Settle",           "Atlantis",           ACT_ATLANTIS, false, q_atl_sq08, "SQ08Complete" },
    { "The Lost Wanderer",           "Gadir Outskirts",    ACT_ATLANTIS, false, q_atl_sq01, "SQ01Complete" },
    { "Nightly Guests",              "Gaulos Wilderness",  ACT_ATLANTIS, false, q_atl_sq07, "SQ07Complete" },
    { "The Letter",                  "Atlas Mountains",    ACT_ATLANTIS, false, q_atl_sq10, "SQ10Complete" },
    { "The Secret Depot",            "Atlas Mountains",    ACT_ATLANTIS, false, q_atl_sq04, "SQ04Complete" },
    { "Of Goats and Bards",          "Atlas Mountains",    ACT_ATLANTIS, false, q_atl_sq03, "SQ03Complete" },
    { "The Foraging Mission",        "Mud Shoals",         ACT_ATLANTIS, false, q_atl_sq06, "SQ06Complete" },
    { "Friends Like These",          "Mud Shoals",         ACT_ATLANTIS, false, q_atl_sq11, "SQ11Complete" },

    /* === ACT VII: Eternal Embers === */
    /* Main quests (x4tagMq##_Title) */
    { "A Strange Land",              NULL, ACT_ETERNAL_EMBERS, true,  q_ee_jc001, "x4MQ_JC001_Completed" },
    { "Seeking An Audience",         NULL, ACT_ETERNAL_EMBERS, true,  q_ee_jc002, "x4MQ_JC002_Completed" },
    { "A Familiar Face",             NULL, ACT_ETERNAL_EMBERS, true,  q_ee_jc003, "x4MQ_JC003_Completed" },
    { "A God No More",               NULL, ACT_ETERNAL_EMBERS, true,  q_ee_je001, "x4MQ_JE001_Completed" },
    /* Side quests (x4tagSq###_Title) */
    /* Side quests — ordered to match in-game quest log (testdata/quests.txt) */
    { "The Root of the Problem",     "Mainland China", ACT_ETERNAL_EMBERS, false, q_ee_sq101, "x4SQ101_Completed" },
    { "Monkey Business",             "Mainland China", ACT_ETERNAL_EMBERS, false, q_ee_sq105, "x4SQ105_QuestCompleted" },
    { "A Book and its Cover",        "Mainland China", ACT_ETERNAL_EMBERS, false, q_ee_sq102, "x4SQ102_Completed" },
    { "The Council of Three",        "Mainland China", ACT_ETERNAL_EMBERS, false, q_ee_sq104, "x4SQ104_Completed" },
    { "Summer Residence Idol",       "Mainland China", ACT_ETERNAL_EMBERS, false, q_ee_ss001, "x4SS001_QiongQi_SummerResidence_Done" },
    { "Emperor Yao's Bell",          "Pingyang",       ACT_ETERNAL_EMBERS, false, q_ee_sq201, "x4SQ201_QuestCompleted" },
    { "Love and Loss",               "Pingyang",       ACT_ETERNAL_EMBERS, false, q_ee_sq202, "x4SQ202_Quest_Finished" },
    { "Lust for Jade",               "Pingyang",       ACT_ETERNAL_EMBERS, false, q_ee_sq203, "x4SQ203_QuestCompleted" },
    { "Fire and Gold",               "Pingyang",       ACT_ETERNAL_EMBERS, false, q_ee_sq204, "x4SQ204_QuestCompleted" },
    { "Heavenly Lovers",             "The Heavens",    ACT_ETERNAL_EMBERS, false, q_ee_sq205, "x4SQ205_Quest_Completed" },
    { "Maze of Mirages",             "The Heavens",    ACT_ETERNAL_EMBERS, false, q_ee_sq206, "x4SQ206_QuestFinished" },
    { "Shadowstone of Anubis",       "Lower Egypt",    ACT_ETERNAL_EMBERS, false, q_ee_sq301, "x4SQ301_QuestReturned" },
    { "No Peace for the Fallen",     "Lower Egypt",    ACT_ETERNAL_EMBERS, false, q_ee_sq302, "x4SQ302_QuestFinished" },
    { "A Venom Most Foul",           "Lower Egypt",    ACT_ETERNAL_EMBERS, false, q_ee_sq303, "x4SQ303_QuestFinished" },
    { "The Phoenix and the Frog",    "The Marshland",  ACT_ETERNAL_EMBERS, false, q_ee_sq401, "x4SQ401_Quest_Completed" },
    { "Just Deserts",                "The Ravaged Square", ACT_ETERNAL_EMBERS, false, q_ee_sq402, "x4SQ402_QuestFinished" },
};

static const int quest_defs_count = sizeof(quest_defs) / sizeof(quest_defs[0]);

const QuestDef *quest_get_defs(int *count_out) {
    if (count_out) *count_out = quest_defs_count;
    return quest_defs;
}

/* ── Checklist extra definitions (non-quest achievements) ─────────────── */
/*
 * Boss chests: BossChest_* on Normal, xBossChest_* on Epic/Legendary.
 * Some IT bosses (Cerberus, Greys, Skeletal Typhon) only have xBossChest_ variants.
 * EE boss chests use unique prefixed tokens (no Normal/Epic split).
 */

static const ChecklistExtraDef checklist_extra_defs[] = {
    /* ── Boss Chests: Greece ── */
    { "Satyr Shaman \u2014 Laconia Hills Cave",           "BossChest_SatyrShaman",       "xBossChest_SatyrShaman",       CHECK_CAT_BOSS_CHEST, ACT_GREECE },
    { "Nessus the Centaur \u2014 Olive Groves",           "BossChest_Nessus",            "xBossChest_Nessus",            CHECK_CAT_BOSS_CHEST, ACT_GREECE },
    { "The Gorgon Sisters \u2014 Knossos Labyrinth",      "BossChest_Gorgons",           "xBossChest_Gorgons",           CHECK_CAT_BOSS_CHEST, ACT_GREECE },
    { "Hydra \u2014 Athens Catacombs",                     "BossChest_Hydra",             "xBossChest_Hydra",             CHECK_CAT_BOSS_CHEST, ACT_GREECE },
    { "Polyphemus the Cyclops \u2014 Monster Encampment",  "BossChest_Cyclops",           "xBossChest_Cyclops",           CHECK_CAT_BOSS_CHEST, ACT_GREECE },
    { "Talos the Bronze Giant \u2014 Knossos Beach",       "BossChest_Talos",             "xBossChest_Talos",             CHECK_CAT_BOSS_CHEST, ACT_GREECE },

    /* ── Boss Chests: Egypt ── */
    { "Scarabaeus the Desert King \u2014 Fayum Desert",    "BossChest_Scarabaeus",   "xBossChest_Scarabaeus",        CHECK_CAT_BOSS_CHEST, ACT_EGYPT },
    { "Sand Wraith Lord \u2014 Tomb of Ramses",            "BossChest_SandWraithLord",    "xBossChest_SandWraithLord",    CHECK_CAT_BOSS_CHEST, ACT_EGYPT },
    { "Scorpos the Scorpion King \u2014 Giza Plateau",     "BossChest_ScorposKing",       "xBossChest_ScorposKing",       CHECK_CAT_BOSS_CHEST, ACT_EGYPT },
    { "Pharaoh's Honor Guard \u2014 The Great Pyramid",    "BossChest_PharaohHonorGuard", "xBossChest_PharaohHonorGuard", CHECK_CAT_BOSS_CHEST, ACT_EGYPT },
    { "Manticore \u2014 Valley of the Kings",              "BossChest_Manticore",         "xBossChest_Manticore",         CHECK_CAT_BOSS_CHEST, ACT_EGYPT },

    /* ── Boss Chests: Orient ── */
    { "Bandari \u2014 Silk Road Highlands",                "BossChest_Bandari",           "xBossChest_Bandari",           CHECK_CAT_BOSS_CHEST, ACT_ORIENT },
    { "Barmanu the Yeti \u2014 Mountain Pass",             "BossChest_Barmanu",           "xBossChest_Barmanu",           CHECK_CAT_BOSS_CHEST, ACT_ORIENT },
    { "Dragon Liche \u2014 Great Wall",                    "BossChest_DragonLiche",       "xBossChest_DragonLiche",       CHECK_CAT_BOSS_CHEST, ACT_ORIENT },
    { "Giant Yeti \u2014 Mountain Cave",                   "BossChest_GiantYeti",         "xBossChest_GiantYeti",         CHECK_CAT_BOSS_CHEST, ACT_ORIENT },
    { "Xiao \u2014 Jade Palace",                           "BossChest_Xiao",              "xBossChest_Xiao",              CHECK_CAT_BOSS_CHEST, ACT_ORIENT },
    { "Yaoguai \u2014 Wusao Mountain",                     "BossChest_Yaoguai",           "xBossChest_Yaoguai",           CHECK_CAT_BOSS_CHEST, ACT_ORIENT },

    /* ── Boss Chests: Immortal Throne ── */
    { "Alastor \u2014 Medea's Grove",                      "BossChest_Alastor",           "xBossChest_Alastor",           CHECK_CAT_BOSS_CHEST, ACT_IMMORTAL_THRONE },
    { "Arachne the Spider Queen \u2014 Spider Cavern",     "BossChest_Arachne",           "xBossChest_Arachne",           CHECK_CAT_BOSS_CHEST, ACT_IMMORTAL_THRONE },
    { "Chimera \u2014 Tower of Judgment",                  "BossChest_Chimera",           "xBossChest_Chimera",           CHECK_CAT_BOSS_CHEST, ACT_IMMORTAL_THRONE },
    { "Cerberus \u2014 River Styx",                        NULL,                          "xBossChest_Cerberos",          CHECK_CAT_BOSS_CHEST, ACT_IMMORTAL_THRONE },
    { "Skeletal Typhon \u2014 Palace of Hades",            NULL,                          "xBossChest_SkeletalTyphon",    CHECK_CAT_BOSS_CHEST, ACT_IMMORTAL_THRONE },
    { "The Grey Sisters \u2014 Fields of Elysium",         NULL,                          "xBossChest_Greys",             CHECK_CAT_BOSS_CHEST, ACT_IMMORTAL_THRONE },

    /* ── Boss Chests: Eternal Embers (no Normal/Epic split) ── */
    { "Terracotta Elite \u2014 Qin Shi Huang's Tomb",     "x4MQJC002_BossChest_TerracottaElite", NULL,                   CHECK_CAT_BOSS_CHEST, ACT_ETERNAL_EMBERS },
    { "Fa Hai the Sorcerer \u2014 Mainland China",         "x4SQ102_BossChest_FaHai",             NULL,                   CHECK_CAT_BOSS_CHEST, ACT_ETERNAL_EMBERS },
    { "Sanshou the Beast \u2014 Mainland China",           "x4SQ104_BossChest_Sanshou",           NULL,                   CHECK_CAT_BOSS_CHEST, ACT_ETERNAL_EMBERS },
    { "Gorilla Shaman \u2014 Mainland China",              "x4SQ105_BossChest_GorillaShaman",     NULL,                   CHECK_CAT_BOSS_CHEST, ACT_ETERNAL_EMBERS },
    { "Li Qiang the Bandit \u2014 Pingyang",               "x4SQ202_BossChest_LiQiang",           NULL,                   CHECK_CAT_BOSS_CHEST, ACT_ETERNAL_EMBERS },
    { "Mogwai Taskmaster \u2014 Pingyang",                 "x4SQ203_BossChest_MogwaiTaskmaster",  NULL,                   CHECK_CAT_BOSS_CHEST, ACT_ETERNAL_EMBERS },
    { "Huoshen the Fire Spirit \u2014 Pingyang",           "x4SQ204_BossChest_Huoshen",           NULL,                   CHECK_CAT_BOSS_CHEST, ACT_ETERNAL_EMBERS },
    { "Looter Leader \u2014 Lower Egypt",                  "x4SQ303_BossChest_LooterLeader",      NULL,                   CHECK_CAT_BOSS_CHEST, ACT_ETERNAL_EMBERS },

    /* ── Exploration ── */
    { "1st Inventory Bag",         "1stBagGiven",              NULL, CHECK_CAT_EXPLORATION, ACT_GREECE },
    { "2nd Inventory Bag",         "2ndBagGiven",              NULL, CHECK_CAT_EXPLORATION, ACT_EGYPT },
    { "3rd Inventory Bag",         "3rdBagGiven",              NULL, CHECK_CAT_EXPLORATION, ACT_ORIENT },
    { "Map Unlock: Orient",        "MapUnlockOrient",          NULL, CHECK_CAT_EXPLORATION, ACT_ORIENT },
    { "Map Unlock: Atlantis",      "MapUnlockAtlantis",        NULL, CHECK_CAT_EXPLORATION, ACT_ATLANTIS },
    { "Tartarus Portal Active",    "x3TartPortalActive",       NULL, CHECK_CAT_EXPLORATION, ACT_ATLANTIS },
    { "Difficulty: Legendary",     "DEV_Difficulty_LEGENDARY",  NULL, CHECK_CAT_EXPLORATION, ACT_GREECE },
    { "Marshland Unlocked",        "x4MQ_JC003_MarshlandUnlocked", NULL, CHECK_CAT_EXPLORATION, ACT_ETERNAL_EMBERS },

    /* ── NPCs ── */
    { "Alala Known",               "AlalaKnown",               NULL, CHECK_CAT_NPC, ACT_GREECE },
    { "Kyros Changed",             "KyrosChanged",             NULL, CHECK_CAT_NPC, ACT_GREECE },
    { "Leonidas Known",            "LeonidasKnown",            NULL, CHECK_CAT_NPC, ACT_GREECE },
    { "Lysander Known",            "LysanderKnown",            NULL, CHECK_CAT_NPC, ACT_GREECE },
    { "Lysander Changed",          "LysanderChanged",          NULL, CHECK_CAT_NPC, ACT_GREECE },
    { "Oracle Changed",            "OracleChanged",            NULL, CHECK_CAT_NPC, ACT_GREECE },
    { "Phaedrus Known",            "PhaedrusKnown",            NULL, CHECK_CAT_NPC, ACT_GREECE },
    { "Timon Known",               "TimonKnown",               NULL, CHECK_CAT_NPC, ACT_EGYPT },
    { "Timon Changed",             "TimonChanged",             NULL, CHECK_CAT_NPC, ACT_EGYPT },
    { "Imhotep Known",             "ImhotepKnown",             NULL, CHECK_CAT_NPC, ACT_EGYPT },
    { "Yellow Emperor Known",      "YellowEmperorKnown",       NULL, CHECK_CAT_NPC, ACT_ORIENT },
    { "Wodbald Talked To",         "Teleporter_WodbaldTalkedToInScandia", NULL, CHECK_CAT_NPC, ACT_RAGNAROK },

    /* ── Miscellaneous ── */
    { "Helos Bridge Scene",            "SS_Helos_BridgeSceneFired",           NULL, CHECK_CAT_MISC, ACT_GREECE },
    { "Helos Side Gate Attack",        "SS_Helos_SideGateAttackProxiesDisabled", NULL, CHECK_CAT_MISC, ACT_GREECE },
    { "Athens Stairs Soldiers Sent",   "SS_AthensStairs_LastSoldiersSent",    NULL, CHECK_CAT_MISC, ACT_GREECE },
    { "Attempted Greek Back Door",     "SS_AttemptedGreekBackDoor",           NULL, CHECK_CAT_MISC, ACT_GREECE },
    { "Cyclops Move Two Ready",        "SS_CyclopsReadyForMoveTwo",          NULL, CHECK_CAT_MISC, ACT_GREECE },
    { "Scroll Found Conversation",     "x2SFConversationDone",               NULL, CHECK_CAT_MISC, ACT_IMMORTAL_THRONE },
    { "Scroll Found Exclamation",      "x2SFExclamationDone",                NULL, CHECK_CAT_MISC, ACT_IMMORTAL_THRONE },
    { "Scroll Found Ambience",         "x2SFTriggerAmb",                     NULL, CHECK_CAT_MISC, ACT_IMMORTAL_THRONE },
    { "HC Dungeon: Door Unlocked",     "x4_HCDung_E_Door_Unlocked",         NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "HC Dungeon: Key A Unlocked",    "x4_HCDung_E_Key_A_Unlocked",        NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "HC Dungeon: Key B Unlocked",    "x4_HCDung_E_Key_B_Unlocked",        NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "HC Dungeon: Key C Unlocked",    "x4_HCDung_E_Key_C_Unlocked",        NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "HC Dungeon: Airlock Entered",   "x4_HCDung_E_Airlock_All_Inside",    NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "HC Dungeon: Airlock Exited",    "x4_HCDung_E_Airlock_All_Outside",   NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "HC Dungeon: Gather Triggered",  "x4_HCDung_E_GatherTriggered",       NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "Achievement: Completed EE",     "x4_ACH14_Completed_EternalEmbers",   NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "All Devs Killed",               "x4_Other_All_Devs_Killed",           NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "Fei Yi Killed",                 "x4_Other_FeiYi_Killed",              NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "Sun's Arena Door Closed",       "x4_Other_SunsArenaDoor_ClosedOnce",  NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
    { "Talked With Jun",               "x4_Other_TalkedWithJun",             NULL, CHECK_CAT_MISC, ACT_ETERNAL_EMBERS },
};

static const int checklist_extra_count = sizeof(checklist_extra_defs) / sizeof(checklist_extra_defs[0]);

const ChecklistExtraDef *checklist_get_extras(int *count_out) {
    if (count_out) *count_out = checklist_extra_count;
    return checklist_extra_defs;
}

const char *checklist_category_name(ChecklistCategory cat) {
    static const char *names[] = {
        "Boss Chests", "Exploration", "NPCs", "Miscellaneous"
    };
    if (cat >= 0 && cat < NUM_CHECK_CATEGORIES) return names[cat];
    return "Unknown";
}
