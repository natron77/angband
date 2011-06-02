/*
 * File: main-stats.c
 * Purpose: Pseudo-UI for stats generation (borrows heavily from main-test.c)
 *
 * Copyright (c) 2010-11 Robert Au <myshkin+angband@durak.net>
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "birth.h"
#include "buildid.h"
#include "init.h"
#include "object/pval.h"
#include "object/tvalsval.h"
#include "stats/db.h"
#include <stddef.h>

#define OBJ_FEEL_MAX	 11
#define MON_FEEL_MAX 	 10
#define LEVEL_MAX 		101
#define TOP_DICE		 21 /* highest catalogued values for wearables */
#define TOP_SIDES		 11
#define TOP_AC			146
#define TOP_PLUS		 56
#define TOP_POWER		999
#define TOP_PVAL		 25
#define RUNS_PER_CHECKPOINT	10000

/* For ref, e_max is 128, a_max is 136, r_max is ~650,
	ORIGIN_STATS is 14, OF_MAX is ~120 */

/* There are 416 kinds, of which about 150-200 are wearable */

static int randarts = 0;
static u32b num_runs = 1;
static int verbose = 0;
static int nextkey = 0;
static int running_stats = 0;
static char *ANGBAND_DIR_STATS;

static ang_file  *ainfo_fp, *rinfo_fp, *finfo_fp, *dinfo_fp;
/* *obj_fp, *mon_fp, */

static int *consumables_index;
static int *wearables_index;
static int *pval_flags_index;
static int wearable_count = 0;
static int consumable_count = 0;
static int pval_flags_count = 0;

struct wearables_data {
	u32b count;
	u32b dice[TOP_DICE][TOP_SIDES];
	u32b ac[TOP_AC];
	u32b hit[TOP_PLUS];
	u32b dam[TOP_PLUS];
/*	u32b power[TOP_POWER]; not enough memory - add it later in bands */
	u32b *egos;
	u32b flags[OF_MAX];
	u32b *pval_flags[TOP_PVAL];
};

static struct level_data {
	u32b *monsters;
/*  u32b *vaults;  Add these later - requires passing into generate.c
	u32b *pits; */
	u32b obj_feelings[OBJ_FEEL_MAX];
	u32b mon_feelings[MON_FEEL_MAX];
	long long gold[ORIGIN_STATS];
	u32b *artifacts[ORIGIN_STATS];
	u32b *consumables[ORIGIN_STATS];
	struct wearables_data *wearables[ORIGIN_STATS];
} level_data[LEVEL_MAX];

static void create_indices()
{
	int i;

	consumables_index = C_ZNEW(z_info->k_max, int);
	wearables_index = C_ZNEW(z_info->k_max, int);
	pval_flags_index = C_ZNEW(OF_MAX, int);

	for (i = 0; i < z_info->k_max; i++) {

		object_type object_type_body;
		object_type *o_ptr = &object_type_body;
		object_kind *kind = &k_info[i];
		o_ptr->tval = kind->tval;

		if (wearable_p(o_ptr))
			wearables_index[i] = ++wearable_count;
		else
			consumables_index[i] = ++consumable_count;
	}

	for (i = 0; i < OF_MAX; i++)
		if (flag_uses_pval(i))
			pval_flags_index[i] = ++pval_flags_count;
}

static void alloc_memory()
{
	int i, j, k, l;

	for (i = 0; i < LEVEL_MAX; i++) {
		level_data[i].monsters = C_ZNEW(z_info->r_max, u32b);
/*		level_data[i].vaults = C_ZNEW(z_info->v_max, u32b);
		level_data[i].pits = C_ZNEW(z_info->pit_max, u32b); */

		for (j = 0; j < ORIGIN_STATS; j++) {
			level_data[i].artifacts[j] = C_ZNEW(z_info->a_max, u32b);
			level_data[i].consumables[j] = C_ZNEW(consumable_count + 1, u32b);
			level_data[i].wearables[j]
				= C_ZNEW(wearable_count + 1, struct wearables_data);

			for (k = 0; k < wearable_count + 1; k++) {
				level_data[i].wearables[j][k].egos
					= C_ZNEW(z_info->e_max, u32b);
				for (l = 0; l < TOP_PVAL; l++)
					level_data[i].wearables[j][k].pval_flags[l]
						= C_ZNEW(pval_flags_count + 1, u32b);
			}
		}
	}
}

static void free_stats_memory(void)
{
	int i, j, k, l;
	for (i = 0; i < LEVEL_MAX; i++) {
		mem_free(level_data[i].monsters);
/*		mem_free(level_data[i].vaults);
 		mem_free(level_data[i].pits); */
		for (j = 0; j < ORIGIN_STATS; j++) {
			mem_free(level_data[i].artifacts[j]);
			mem_free(level_data[i].consumables[j]);
			for (k = 0; k < wearable_count + 1; k++) {
				for (l = 0; l < TOP_PVAL; l++) {
					mem_free(level_data[i].wearables[j][k].pval_flags[l]);
				}
				mem_free(level_data[i].wearables[j][k].egos);
			}
			mem_free(level_data[i].wearables[j]);
		}
	}
	mem_free(consumables_index);
	mem_free(wearables_index);
	mem_free(pval_flags_index);
}

/* Copied from birth.c:generate_player() */
static void generate_player_for_stats()
{
	OPT(birth_randarts) = randarts;
	OPT(birth_no_stacking) = FALSE;
	OPT(auto_more) = TRUE;

	p_ptr->wizard = 1; /* Set wizard mode on */

	p_ptr->psex = 0;   /* Female  */
	p_ptr->race = races;  /* Human   */
	p_ptr->class = classes; /* Warrior */

	/* Level 1 */
	p_ptr->max_lev = p_ptr->lev = 1;

	/* Experience factor */
	p_ptr->expfact = p_ptr->race->r_exp + p_ptr->class->c_exp;

	/* Hitdice */
	p_ptr->hitdie = p_ptr->race->r_mhp + p_ptr->class->c_mhp;

	/* Initial hitpoints -- high just to be safe */
	p_ptr->mhp = p_ptr->chp = 2000;

	/* Pre-calculate level 1 hitdice */
	p_ptr->player_hp[0] = p_ptr->hitdie;

	/* Set age/height/weight */
	p_ptr->ht = p_ptr->ht_birth = 66;
	p_ptr->wt = p_ptr->wt_birth = 150;
	p_ptr->age = 14;

	/* Set social class and (null) history */
	p_ptr->history = get_history(p_ptr->race->history, &p_ptr->sc);
	p_ptr->sc_birth = p_ptr->sc;
}

static void initialize_character(void)
{
	u32b seed;

	seed = (time(NULL));
	Rand_quick = FALSE;
	Rand_state_init(seed);

	player_init(p_ptr);
	generate_player_for_stats();

	seed_flavor = randint0(0x10000000);
	seed_town = randint0(0x10000000);
	seed_randart = randint0(0x10000000);

	if (randarts)
	{
		do_randart(seed_randart, TRUE);
	}

	store_init();
	flavor_init();
	p_ptr->playing = TRUE;
	p_ptr->autosave = FALSE;
	cave_generate(cave, p_ptr);
}

static void kill_all_monsters(int level)
{
	int i;

	for (i = cave_monster_max(cave) - 1; i >= 1; i--) {
		const monster_type *m_ptr = cave_monster(cave, i);
		monster_race *r_ptr = &r_info[m_ptr->r_idx];

		level_data[level].monsters[m_ptr->r_idx]++;

		monster_death(i, TRUE);

		if (rf_has(r_ptr->flags, RF_UNIQUE))
			r_ptr->max_num = 0;
	}
}

static void unkill_uniques(void)
{
	int i;

	for (i = 0; i < z_info->r_max; i++) {
		monster_race *r_ptr = &r_info[i];

		if (rf_has(r_ptr->flags, RF_UNIQUE))
			r_ptr->max_num = 1;
	}
}

static void reset_artifacts(void)
{
	int i;

	for (i = 0; i < z_info->a_max; i++)
		a_info[i].created = FALSE;

}

/*
 * Insert into out_str a hex representation of the bitflag flags[].
 */
static void flag2hex(const bitflag flags[], char *out_str)
{
	unsigned int i;

	out_str[2*OF_SIZE] = 0;

	for (i = 0; i < OF_SIZE; i++)
		strnfmt(out_str + 2 * i, 2 * OF_SIZE - 2 * i + 1, "%02x", flags[i]);
}

/*
 * Insert into pvals a comma-separated list of pvals in pval[],
 * and insert into pval_flags a comma-separated list of hex
 * representations of the bitflags in pval_flags[].
 */

static void dump_pvals(char *pval_string, char *pval_flag_string,
	s16b pval[], bitflag pval_flags[][OF_SIZE], byte num_pvals)
{
	unsigned int i;
	size_t pval_end = 0;
	size_t pval_flag_end = 0;
	char buf[64];

	if (num_pvals <= 0) return;

	for (i = 0; (i + 1) < num_pvals; i++)
	{
		strnfcat(pval_string, 20, &pval_end, "%d,", pval[i]);
		flag2hex(pval_flags[i], buf);
		strnfcat(pval_flag_string, 128, &pval_flag_end, "%s,", buf);
	}

	strnfcat(pval_string, 20, &pval_end, "%d", pval[num_pvals - 1]);
	flag2hex(pval_flags[num_pvals - 1], buf);
	strnfcat(pval_flag_string, 128, &pval_flag_end, "%s", buf);
}

static void log_all_objects(int level)
{
	int x, y, i;

	for (y = 1; y < DUNGEON_HGT - 1; y++) {
		for (x = 1; x < DUNGEON_WID - 1; x++) {
			object_type *o_ptr = get_first_object(y, x);

			if (o_ptr) do {
			/*	u32b o_power = 0; */

				/* Mark object as fully known */
				object_notice_everything(o_ptr);

/*				o_power = object_power(o_ptr, FALSE, NULL, TRUE); */

				/* Capture gold amounts */
				if (o_ptr->tval == TV_GOLD)
					level_data[level].gold[o_ptr->origin] += o_ptr->pval[DEFAULT_PVAL];

				/* Capture artifact drops */
				if (o_ptr->artifact)
					level_data[level].artifacts[o_ptr->origin][o_ptr->artifact->aidx]++;

				/* Capture kind details */
				if (wearable_p(o_ptr)) {
					struct wearables_data *w
						= &level_data[level].wearables[o_ptr->origin][wearables_index[o_ptr->kind->kidx]];

					w->count++;
					w->dice[MIN(o_ptr->dd, TOP_DICE - 1)][MIN(o_ptr->ds, TOP_SIDES - 1)]++;
					w->ac[MIN(MAX(o_ptr->ac + o_ptr->to_a, 0), TOP_AC - 1)]++;
					w->hit[MIN(MAX(o_ptr->to_h, 0), TOP_PLUS - 1)]++;
					w->dam[MIN(MAX(o_ptr->to_d, 0), TOP_PLUS - 1)]++;

					/* Capture egos */
					if (o_ptr->ego)
						w->egos[o_ptr->ego->eidx]++;
					/* Capture object flags */
					for (i = of_next(o_ptr->flags, FLAG_START); i != FLAG_END;
							i = of_next(o_ptr->flags, i + 1)) {
						w->flags[i]++;
						if (flag_uses_pval(i)) {
							int p = o_ptr->pval[which_pval(o_ptr, i)];
							w->pval_flags[MIN(MAX(p, 0), TOP_PVAL - 1)][pval_flags_index[i]]++;
						}
					}
				} else
					level_data[level].consumables[o_ptr->origin][consumables_index[o_ptr->kind->kidx]]++;
			}
			while ((o_ptr = get_next_object(o_ptr)));
		}
	}
}

static void open_output_files(u32b run)
{
	char buf[1024];
	char run_dir[1024];

	strnfmt(run_dir, 1024, "%s%s%010d", ANGBAND_DIR_STATS, PATH_SEP, run);

	if (!dir_create(run_dir))
	{
		quit("Couldn't create stats run directory!");
	}

/*	path_build(buf, sizeof(buf), run_dir, "objects.txt");
	obj_fp = file_open(buf, MODE_WRITE, FTYPE_TEXT);
	path_build(buf, sizeof(buf), run_dir, "monsters.txt");
	mon_fp = file_open(buf, MODE_WRITE, FTYPE_TEXT); */
	path_build(buf, sizeof(buf), run_dir, "ainfo.txt");
	ainfo_fp = file_open(buf, MODE_WRITE, FTYPE_TEXT);
	path_build(buf, sizeof(buf), run_dir, "rinfo.txt");
	rinfo_fp = file_open(buf, MODE_WRITE, FTYPE_TEXT);
	path_build(buf, sizeof(buf), run_dir, "feelings.txt");
	finfo_fp = file_open(buf, MODE_WRITE, FTYPE_TEXT);
	path_build(buf, sizeof(buf), run_dir, "datatest.txt");
	dinfo_fp = file_open(buf, MODE_WRITE, FTYPE_TEXT);

	/* Print headers */
/*	file_putf(obj_fp, "tval|sval|pvals|name1|name2|number|origin|origin_depth|origin_xtra|to_h|to_d|to_a|ac|dd|ds|weight|flags|pval_flags|power|name\n");
	file_putf(mon_fp, "level|r_idx|name\n"); */
	file_putf(ainfo_fp, "aidx|tval|sval|pvals|to_h|to_d|to_a|ac|dd|ds|weight|flags|pval_flags|level|alloc_prob|alloc_min|alloc_max|effect|name\n");
	file_putf(rinfo_fp, "ridx|level|rarity|d_char|name\n");
	file_putf(finfo_fp, "Level feelings (%d runs):\n", num_runs);
	file_putf(dinfo_fp, "Sample results from the level_data structure:\n");
}

static void close_output_files(void)
{
/*	file_close(obj_fp);
	file_close(mon_fp);*/
	file_close(ainfo_fp);
	file_close(rinfo_fp);
	file_close(finfo_fp);
	file_close(dinfo_fp);
}

static void dump_ainfo(void)
{
	unsigned int i;

	for (i = 0; i < z_info->a_max; i++)
	{
		artifact_type *a_ptr = &a_info[i];
		char a_flags[128] = "";
		char a_pvals[20] = "";
		char a_pval_flags[128] = "";

		/* Don't print anything for "empty" artifacts */
		if (!a_ptr->name) continue;

		flag2hex(a_ptr->flags, a_flags);

		if (a_ptr->num_pvals > 0)
			dump_pvals(a_pvals, a_pval_flags, a_ptr->pval,
				a_ptr->pval_flags, a_ptr->num_pvals);

		file_putf(ainfo_fp,
			"%d|%d|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d|%s|%s|%d|%d|%d|%d|%d|%s\n",
			a_ptr->aidx,
			a_ptr->tval,
			a_ptr->sval,
			a_pvals,
			a_ptr->to_h,
			a_ptr->to_d,
			a_ptr->to_a,
			a_ptr->ac,
			a_ptr->dd,
			a_ptr->ds,
			a_ptr->weight,
			a_ptr->cost,
			a_flags,
			a_pval_flags,
			a_ptr->level,
			a_ptr->alloc_prob,
			a_ptr->alloc_min,
			a_ptr->alloc_max,
			a_ptr->effect,
			a_ptr->name);
	}
}

static void dump_rinfo(void)
{
	unsigned int i;

	for (i = 0; i < z_info->r_max; i++)
	{
		monster_race *r_ptr = &r_info[i];

		/* Don't print anything for "empty" artifacts */
		if (!r_ptr->name) continue;

	        /* ridx|level|rarity|d_char|name */
		file_putf(rinfo_fp,
			"%d|%d|%d|%c|%s\n",
			r_ptr->ridx,
			r_ptr->level,
			r_ptr->rarity,
			r_ptr->d_char,
			r_ptr->name);
	}
}

static void dump_feelings(void)
{
	int i, j;

	for (j = 1; j < LEVEL_MAX; j++) {
		for (i = 0; i < OBJ_FEEL_MAX; i++)
			file_putf(finfo_fp, "Level %d obj_feeling %d: %d\n", j, i,
				level_data[j].obj_feelings[i]);
		for (i = 0; i < MON_FEEL_MAX; i++)
			file_putf(finfo_fp, "Level %d mon_feeling %d: %d\n", j, i,
				level_data[j].mon_feelings[i]);
	}
}

static void dump_results(void)
{
	int i;
	int lev = randint1(LEVEL_MAX - 1);

	file_putf(dinfo_fp, "Sample floor data from level %d:\n", lev);

	for (i = 0; i < ORIGIN_STATS; i++)
		file_putf(dinfo_fp, "Gold from origin %d: %d\n", i, level_data[lev].gold[i]);

	for (i = 0; i < consumable_count + 1; i++)
		file_putf(dinfo_fp, "Consumable %d: %d\n", i,
			level_data[lev].consumables[ORIGIN_FLOOR][i]);

	for (i = 0; i < wearable_count + 1; i++)
		file_putf(dinfo_fp, "Wearable %d: %d\n", i,
			level_data[lev].wearables[ORIGIN_FLOOR][i].count);

}

static void descend_dungeon(void)
{
	int level;
	u16b obj_f, mon_f;

	for (level = 1; level < LEVEL_MAX; level++)
	{
		dungeon_change_level(level);
		cave_generate(cave, p_ptr);

		/* Store level feelings */
		obj_f = cave->feeling / 10;
		mon_f = cave->feeling - (10 * obj_f);
		level_data[level].obj_feelings[MIN(obj_f, OBJ_FEEL_MAX - 1)]++;
		level_data[level].mon_feelings[MIN(mon_f, MON_FEEL_MAX - 1)]++;

		kill_all_monsters(level);
		log_all_objects(level);
	}
}

static void prep_output_dir(void)
{
	size_t size = strlen(ANGBAND_DIR_USER) + strlen(PATH_SEP) + 6;
	ANGBAND_DIR_STATS = mem_alloc(size);
	strnfmt(ANGBAND_DIR_STATS, size,
		"%s%sstats", ANGBAND_DIR_USER,PATH_SEP);

	if (dir_create(ANGBAND_DIR_STATS))
	{
		return;
	}
	else
	{
		quit("Couldn't create stats directory!");
	}
}

static int stats_dump_tables(void)
{
	return SQLITE_OK;
}

/**
 * Open the database connection and create the database tables.
 * All count tables will contain a level column (INTEGER) and a
 * count column (INTEGER). Some tables will include other INTEGER columns
 * for object origin, feeling, attributes, indices, or object flags.
 * Tables:
 *     metadata -- key-value pairs describing the stats run
 *     artifact_info -- dump of artifact.txt
 *     artifact_flags_map -- map between artifacts and object flags
 *     artifact_pval_flags_map -- map between artifacts and pval flags, with pvals
 *     ego_info -- dump of ego_item.txt
 *     ego_flags_map -- map between egos and object flags
 *     ego_pval_flags_map -- map between egos and pval flags, with pvals and minima
 *     ego_type_map -- map between egos and tvals/svals
 *     monster_base_info -- dump of monster_base.txt
 *     monster_base_flags_map -- map between monsters and monster flags
 *     monster_base_spell_flags_map -- map between monsters and monster spell flags
 *     monster_info -- dump of monsters.txt
 *     monster_flags_map -- map between monsters and monster flags
 *     monster_spell_flags_map -- map between monsters and monster spell flags
 *     object_base_info -- dump of object_base.txt
 *     object_base_flags_map -- map between object templates and object flags
 *     object_info -- dump of objects.txt
 *     object_flags_map -- map between artifacts and object flags
 *     object_pval_flags_map -- map between artifacts and pval flags, with pvals
 *     effects_list -- dump of list-effects.h
 *     monster_flags_list -- dump of list-mon-flags.h
 *     monster_spell_flags_list -- dump of list-mon-spells.h
 *     object_flags_list -- dump of list-object-flags.h
 *     object_flag_type_list -- dump of object_flag_type enum
 *     object_slays_list -- dump of list-object-slays.h
 *     origin_flags_list -- dump of origin enum
 *     wearables_index -- contains list of wearable object indices
 *     consumables_index -- contains list of consumable object indices
 *     pval_flags_index -- contains list of object flag indices with pvals
 * Count tables:
 *     monsters
 *     obj_feelings
 *     mon_feelings
 *     gold
 *     artifacts
 *     consumables
 *     wearables_count
 *     wearables_dice
 *     wearables_ac
 *     wearables_hit
 *     wearables_dam
 *     wearables_egos
 *     wearables_flags
 *     wearables_pval_flags
 */
static bool stats_prep_db(void)
{
	bool status;
	int err;

	/* Open the database connection */
	status = stats_db_open();
	if (!status) return status;

	/* Create some tables */
	err = stats_db_exec("CREATE TABLE metadata(field TEXT UNIQUE NOT NULL, value TEXT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE artifact_info(idx INT PRIMARY KEY, name TEXT, tval INT, sval INT, weight INT, cost INT, alloc_prob INT, alloc_min INT, alloc_max INT, ac INT, dd INT, ds INT, to_h INT, to_d INT, to_a INT, effect INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE artifact_flags_map(a_idx INT, o_flag INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE artifact_pval_flags_map(a_idx INT, pval_flag INT, pval INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE ego_info(idx INT PRIMARY KEY, name TEXT, cost INT, level INT, rarity INT, rating INT, to_h TEXT, to_d TEXT, to_a TEXT, num_pvals INT, min_to_h INT, min_to_d INT, min_to_a INT, xtra INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE ego_flags_map(e_idx INT, o_flag INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE ego_pval_flags_map(e_idx INT, pval_flag INT, pval TEXT, min_pval INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE ego_type_map(e_idx INT, tval INT, min_sval INT, max_sval INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monster_base_info(idx INT PRIMARY KEY, name TEXT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monster_base_flags_map(rb_idx INT, r_flag INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monster_base_spell_flags_map(rb_idx INT, rs_flag INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monster_info(idx INT PRIMARY KEY, name TEXT, base TEXT, ac INT, sleep INT, speed INT, mexp INT, hp INT, freq_innate INT, freq_spell INT, level INT, rarity INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monster_flags_map(r_idx INT, r_flag INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monster_spell_flags_map(r_idx INT, rs_flag INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE object_base_info(idx INT PRIMARY KEY, name TEXT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE object_base_flags_map(kb_idx INT, o_flag INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE object_info(idx INT PRIMARY KEY, name TEXT, tval INT, sval INT, level INT, weight INT, cost INT, ac INT, dd INT, ds INT, to_h INT, to_d INT, to_a INT, alloc_prob INT, alloc_min INT, alloc_max INT, charge TEXT, effect INT, recharge_time INT, gen_mult_prob INT, stack_size INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE object_flags_map(k_idx INT, o_flag INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE object_pval_flags_map(k_idx INT, pval_flag INT, pval INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE effects_list(idx INT PRIMARY KEY, name TEXT, rating INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monster_flags_list(idx INT PRIMARY KEY, name TEXT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monster_spell_flags_list(idx INT PRIMARY KEY, name TEXT, cap INT, div INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE object_flags_list(idx INT PRIMARY KEY, name TEXT, pval INT, type INT, power INT, pval_mult INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE object_flag_type_list(idx INT PRIMARY KEY, name TEXT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE object_slays_list(idx INT PRIMARY KEY, name TEXT, object_flag INT, monster_flag INT, resist_flag INT, mult INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE origin_flags_list(idx INT PRIMARY KEY, name TEXT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_index(idx INT PRIMARY KEY, k_idx INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE consumables_index(idx INT PRIMARY KEY, k_idx INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE pval_flags_index(idx INT PRIMARY KEY, of_idx INT);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE monsters(level INT, count INT, k_idx INT, UNIQUE (level, k_idx) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE obj_feelings(level INT, count INT, feeling INT, UNIQUE (level, feeling) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE mon_feelings(level INT, count INT, feeling INT, UNIQUE (level, feeling) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE gold(level INT, count INT, origin INT, UNIQUE (level, origin) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE artifacts(level INT, count INT, a_idx INT, origin INT, UNIQUE (level, a_idx, origin) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE consumables(level INT, count INT, c_idx INT, origin INT, UNIQUE (level, c_idx, origin) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_count(level INT, count INT, w_idx INT, origin INT, UNIQUE (level, w_idx, origin) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_dice(level INT, count INT, w_idx INT, origin INT, dd INT, ds INT, UNIQUE (level, w_idx, origin, dd, ds) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_ac(level INT, count INT, w_idx INT, origin INT, ac INT, UNIQUE (level, w_idx, origin, ac) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_hit(level INT, count INT, w_idx INT, origin INT, to_h INT, UNIQUE (level, w_idx, origin, to_h) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_dam(level INT, count INT, w_idx INT, origin INT, to_d INT, UNIQUE (level, w_idx, origin, to_d) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_egos(level INT, count INT, w_idx INT, origin INT, e_idx INT, UNIQUE (level, w_idx, origin, e_idx) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_flags(level INT, count INT, w_idx INT, origin INT, of_idx INT, UNIQUE (level, w_idx, origin, of_idx) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_db_exec("CREATE TABLE wearables_pval_flags(level INT, count INT, w_idx INT, origin INT, pval INT, pv_idx INT, UNIQUE (level, w_idx, origin, pv_idx) ON CONFLICT REPLACE);");
	if (err) return false;

	err = stats_dump_tables();
	if (err) return false;

	return true;
}

/**
 * Find the offset of the given member of the level_data struct. Not elegant.
 */
static int stats_level_data_offsetof(const char *member)
{
	if (streq(member, "monsters"))
		return offsetof(struct level_data, monsters);
	else if (streq(member, "obj_feelings"))
		return offsetof(struct level_data, obj_feelings);
	else if (streq(member, "mon_feelings"))
		return offsetof(struct level_data, mon_feelings);
	else if (streq(member, "gold"))
		return offsetof(struct level_data, gold);
	else if (streq(member, "artifacts"))
		return offsetof(struct level_data, artifacts);
	else if (streq(member, "consumables"))
		return offsetof(struct level_data, consumables);
	else if (streq(member, "wearables"))
		return offsetof(struct level_data, wearables);
		
	/* We should not get to this point. */
	assert(0);
}

/**
 * Find the offset of the given member of the wearables_data struct. Not 
 * elegant.
 */
static int stats_wearables_data_offsetof(const char *member)
{
	if (streq(member, "count"))
		return offsetof(struct wearables_data, count);
	else if (streq(member, "dice"))
		return offsetof(struct wearables_data, dice);
	else if (streq(member, "ac"))
		return offsetof(struct wearables_data, ac);
	else if (streq(member, "hit"))
		return offsetof(struct wearables_data, hit);
	else if (streq(member, "dam"))
		return offsetof(struct wearables_data, dam);
	else if (streq(member, "egos"))
		return offsetof(struct wearables_data, egos);
	else if (streq(member, "flags"))
		return offsetof(struct wearables_data, flags);
	else if (streq(member, "pval_flags"))
		return offsetof(struct wearables_data, pval_flags);
		
	/* We should not get to this point. */
	assert(0);
}

static int stats_write_db_level_data(const char *table, int max_idx)
{
	char sql_buf[256];
	sqlite3_stmt *sql_stmt;
	int err, level, i, offset;

	strnfmt(sql_buf, 256, "INSERT INTO %s VALUES(?,?,?);", table);
	err = stats_db_stmt_prep(&sql_stmt, sql_buf);
	if (err) return err;

	offset = stats_level_data_offsetof(table);

	for (level = 1; level < LEVEL_MAX; level++)
	{
		for (i = 0; i < max_idx; i++)
		{
			/* This arcane expression finds the value of 
			 * level_data[level].<table>[i] */
			u32b count = ((byte *)&level_data[level] + offset)[i];
			if (!count) continue;

			err = stats_db_bind_ints(sql_stmt, 3, 
				level, count, i);
			if (err) return err;

			err = sqlite3_step(sql_stmt);
			if (err && err != SQLITE_DONE) return err;

			err = sqlite3_reset(sql_stmt);
			if (err) return err;
		}
	}

	return sqlite3_finalize(sql_stmt);
}

static int stats_write_db_level_data_items(const char *table, int max_idx)
{
	char sql_buf[256];
	sqlite3_stmt *sql_stmt;
	int err, level, origin, i, offset;

	strnfmt(sql_buf, 256, "INSERT INTO %s VALUES(?,?,?,?);", table);
	err = stats_db_stmt_prep(&sql_stmt, sql_buf);
	if (err) return err;

	offset = stats_level_data_offsetof(table);

	for (level = 1; level < LEVEL_MAX; level++)
	{
		for (origin = 0; origin < ORIGIN_STATS; origin++)
		{
			for (i = 0; i < max_idx; i++)
			{
				/* This arcane expression finds the value of 
				 * level_data[level].<table>[origin][i] */
				u32b count = ((u32b **)((byte *)&level_data[level] + offset))[origin][i];
				if (!count) continue;

				err = stats_db_bind_ints(sql_stmt, 4, 
					level, count, i, origin);
				if (err) return err;

				err = sqlite3_step(sql_stmt);
				if (err && err != SQLITE_DONE) return err;

				err = sqlite3_reset(sql_stmt);
				if (err) return err;
			}
		}
	}

	return sqlite3_finalize(sql_stmt);
}

static int stats_write_db_wearables_count(void)
{
	sqlite3_stmt *sql_stmt;
	int err, level, origin, i;

	err = stats_db_stmt_prep(&sql_stmt, 
		"INSERT INTO wearables_count VALUES(?,?,?,?);");
	if (err) return err;

	for (level = 1; level < LEVEL_MAX; level++)
	{
		for (origin = 0; origin < ORIGIN_STATS; origin++)
		{
			for (i = 0; i < wearable_count + 1; i++)
			{
				u32b count = level_data[level].wearables[origin][i].count;
				if (!count) continue;

				err = stats_db_bind_ints(sql_stmt, 4, 
					level, count, i, origin);
				if (err) return err;

				err = sqlite3_step(sql_stmt);
				if (err && err != SQLITE_DONE) return err;

				err = sqlite3_reset(sql_stmt);
				if (err) return err;
			}
		}
	}

	return sqlite3_finalize(sql_stmt);
}

/**
 * Unfortunately, the arcane expression used to find the value of an array 
 * member of a struct differs depending on whether the member is declared
 * as an array or as a pointer. Pass in true if the member is an array, and
 * false if the member is a pointer.
 */
static int stats_write_db_wearables_array(const char *field, int max_val, bool array_p)
{
	char sql_buf[256];
	sqlite3_stmt *sql_stmt;
	int err, level, origin, idx, i, offset;

	strnfmt(sql_buf, 256, "INSERT INTO wearables_%s VALUES(?,?,?,?,?);", field);
	err = stats_db_stmt_prep(&sql_stmt, sql_buf);
	if (err) return err;

	offset = stats_wearables_data_offsetof(field);

	for (level = 1; level < LEVEL_MAX; level++)
	{
		for (origin = 0; origin < ORIGIN_STATS; origin++)
		{
			for (idx = 0; idx < wearable_count + 1; idx++)
			{
				for (i = 0; i < max_val; i++)
				{
					/* This arcane expression finds the value of
					 * level_data[level].wearables[origin][idx].<field>[i] */
					u32b count;
					if (array_p)
					{
						count = ((u32b *)((byte *)&level_data[level].wearables[origin][idx] + offset))[i];
					}
					else
					{
						count = ((u32b *)*((u32b **)((byte *)&level_data[level].wearables[origin][idx] + offset)))[i];
					}
					if (!count) continue;

					err = stats_db_bind_ints(sql_stmt, 5, 
						level, count, idx, origin, i);
					if (err) return err;

					err = sqlite3_step(sql_stmt);
					if (err && err != SQLITE_DONE) return err;

					err = sqlite3_reset(sql_stmt);
					if (err) return err;
				}
			}
		}
	}

	return sqlite3_finalize(sql_stmt);
}

/**
 * Unfortunately, the arcane expression used to find the value of an array 
 * member of a struct differs depending on whether the member is declared
 * as an array or as a pointer. Pass in true if the member is an array, and
 * false if the member is a pointer.
 */
static int stats_write_db_wearables_2d_array(const char *field, 
	int max_val1, int max_val2, bool array_p)
{
	char sql_buf[256];
	sqlite3_stmt *sql_stmt;
	int err, level, origin, idx, i, j, offset;

	strnfmt(sql_buf, 256, "INSERT INTO wearables_%s VALUES(?,?,?,?,?,?);", field);
	err = stats_db_stmt_prep(&sql_stmt, sql_buf);
	if (err) return err;

	offset = stats_wearables_data_offsetof(field);

	for (level = 1; level < LEVEL_MAX; level++)
	{
		for (origin = 0; origin < ORIGIN_STATS; origin++)
		{
			for (idx = 0; idx < wearable_count + 1; idx++)
			{
				for (i = 0; i < max_val1; i++)
				{
					for (j = 0; j < max_val2; j++)
					{
						/* This arcane expression finds the value of
				 		* level_data[level].wearables[origin][idx].<field>[i][j] */
						u32b count;

						if (array_p)
						{
							count = ((u32b *)((byte *)&level_data[level].wearables[origin][idx] + offset))[i * max_val2 + j];
						}
						else
						{
							count = *(*((u32b **)((byte *)&level_data[level].wearables[origin][idx] + offset) + i) + j);
						}
						if (!count) continue;

						err = stats_db_bind_ints(sql_stmt, 6, 
							level, count, idx, origin, i, j);
						if (err) return err;

						err = sqlite3_step(sql_stmt);
						if (err && err != SQLITE_DONE) return err;

						err = sqlite3_reset(sql_stmt);
						if (err) return err;
					}
				}
			}
		}
	}

	return sqlite3_finalize(sql_stmt);
}

static int stats_write_db(u32b run)
{
	char sql_buf[256];
	int err;

	/* Wrap entire write into a transaction */
	err = stats_db_exec("BEGIN TRANSACTION;");
	if (err) return err;

	strnfmt(sql_buf, 256, 
		"INSERT OR REPLACE INTO metadata VALUES('runs', %d);", run);
	err = stats_db_exec(sql_buf);
	if (err) return err;

	err = stats_write_db_level_data("monsters", z_info->r_max);
	if (err) return err;

	err = stats_write_db_level_data("obj_feelings", OBJ_FEEL_MAX);
	if (err) return err;

	err = stats_write_db_level_data("mon_feelings", MON_FEEL_MAX);
	if (err) return err;

	err = stats_write_db_level_data("gold", ORIGIN_STATS);
	if (err) return err;

	err = stats_write_db_level_data_items("artifacts", z_info->a_max);
	if (err) return err;

	err = stats_write_db_level_data_items("consumables", consumable_count + 1);
	if (err) return err;

	err = stats_write_db_wearables_count();
	if (err) return err;

	err = stats_write_db_wearables_2d_array("dice", TOP_DICE, TOP_SIDES, true);
	if (err) return err;

	err = stats_write_db_wearables_array("ac", TOP_AC, true);
	if (err) return err;

	err = stats_write_db_wearables_array("hit", TOP_PLUS, true);
	if (err) return err;

	err = stats_write_db_wearables_array("dam", TOP_PLUS, true);
	if (err) return err;

	err = stats_write_db_wearables_array("egos", z_info->e_max, false);
	if (err) return err;

	err = stats_write_db_wearables_array("flags", OF_MAX, true);
	if (err) return err;

	err = stats_write_db_wearables_2d_array("pval_flags", TOP_PVAL, pval_flags_count + 1, false);
	if (err) return err;

	/* Commit transaction */
	err = stats_db_exec("COMMIT;");
	if (err) return err;

	return SQLITE_OK;
}

static errr run_stats(void)
{
	u32b run;
	artifact_type *a_info_save;
	unsigned int i;
	int err;

	bool status = stats_prep_db();
	if (!status) quit("Couldn't prepare database!");
	prep_output_dir();
	create_indices();
	alloc_memory();

	if (randarts)
	{
		a_info_save = mem_zalloc(z_info->a_max * sizeof(artifact_type));
		for (i = 0; i < z_info->a_max; i++)
		{
			if (!a_info[i].name) continue;

			memcpy(&a_info_save[i], &a_info[i], sizeof(artifact_type));
		}
	}

	open_output_files(0);

	for (run = 0; run < num_runs; run++)
	{
		if (randarts)
		{
			for (i = 0; i < z_info->a_max; i++)
			{
				memcpy(&a_info[i], &a_info_save[i], sizeof(artifact_type));
			}
		}

		initialize_character();
		unkill_uniques();
		reset_artifacts();
		descend_dungeon();

		/* Checkpoint every so many runs */
		if (run > 0 && run % RUNS_PER_CHECKPOINT == 0)
		{
			err = stats_write_db(run);
			if (err)
			{
				stats_db_close();
				quit_fmt("Problems writing to database!  sqlite3 errno %d.", err);
			}
		}
	}

	err = stats_write_db(run);
	stats_db_close();
	if (err) quit_fmt("Problems writing to database!  sqlite3 errno %d.", err);
	dump_ainfo();
	dump_rinfo();
	dump_feelings();
	dump_results();
	close_output_files();
	free_stats_memory();
	cleanup_angband();
	quit(NULL);
	exit(0);
}

typedef struct term_data term_data;
struct term_data {
	term t;
};

static term_data td;
typedef struct {
	int key;
	errr (*func)(int v);
} term_xtra_func;

static void term_init_stats(term *t) {
	if (verbose) printf("term-init %s %s\n", VERSION_NAME, VERSION_STRING);
}

static void term_nuke_stats(term *t) {
	if (verbose) printf("term-end\n");
}

static errr term_xtra_clear(int v) {
	if (verbose) printf("term-xtra-clear %d\n", v);
	return 0;
}

static errr term_xtra_noise(int v) {
	if (verbose) printf("term-xtra-noise %d\n", v);
	return 0;
}

static errr term_xtra_fresh(int v) {
	if (verbose) printf("term-xtra-fresh %d\n", v);
	return 0;
}

static errr term_xtra_shape(int v) {
	if (verbose) printf("term-xtra-shape %d\n", v);
	return 0;
}

static errr term_xtra_alive(int v) {
	if (verbose) printf("term-xtra-alive %d\n", v);
	return 0;
}

static errr term_xtra_event(int v) {
	if (verbose) printf("term-xtra-event %d\n", v);
	if (nextkey) {
		Term_keypress(nextkey, 0);
		nextkey = 0;
	}
	if (running_stats) {
		return 0;
	}
	running_stats = 1;
	return run_stats();
}

static errr term_xtra_flush(int v) {
	if (verbose) printf("term-xtra-flush %d\n", v);
	return 0;
}

static errr term_xtra_delay(int v) {
	if (verbose) printf("term-xtra-delay %d\n", v);
	return 0;
}

static errr term_xtra_react(int v) {
	if (verbose) printf("term-xtra-react\n");
	return 0;
}

static term_xtra_func xtras[] = {
	{ TERM_XTRA_CLEAR, term_xtra_clear },
	{ TERM_XTRA_NOISE, term_xtra_noise },
	{ TERM_XTRA_FRESH, term_xtra_fresh },
	{ TERM_XTRA_SHAPE, term_xtra_shape },
	{ TERM_XTRA_ALIVE, term_xtra_alive },
	{ TERM_XTRA_EVENT, term_xtra_event },
	{ TERM_XTRA_FLUSH, term_xtra_flush },
	{ TERM_XTRA_DELAY, term_xtra_delay },
	{ TERM_XTRA_REACT, term_xtra_react },
	{ 0, NULL },
};

static errr term_xtra_stats(int n, int v) {
	int i;
	for (i = 0; xtras[i].func; i++) {
		if (xtras[i].key == n) {
			return xtras[i].func(v);
		}
	}
	if (verbose) printf("term-xtra-unknown %d %d\n", n, v);
	return 0;
}

static errr term_curs_stats(int x, int y) {
	if (verbose) printf("term-curs %d %d\n", x, y);
	return 0;
}

static errr term_wipe_stats(int x, int y, int n) {
	if (verbose) printf("term-wipe %d %d %d\n", x, y, n);
	return 0;
}

static errr term_text_stats(int x, int y, int n, byte a, const char *s) {
	if (verbose) printf("term-text %d %d %d %02x %s\n", x, y, n, a, s);
	return 0;
}

static void term_data_link(int i) {
	term *t = &td.t;

	term_init(t, 80, 24, 256);

	/* Ignore some actions for efficiency and safety */
	t->never_bored = TRUE;
	t->never_frosh = TRUE;

	t->init_hook = term_init_stats;
	t->nuke_hook = term_nuke_stats;

	t->xtra_hook = term_xtra_stats;
	t->curs_hook = term_curs_stats;
	t->wipe_hook = term_wipe_stats;
	t->text_hook = term_text_stats;

	t->data = &td;

	Term_activate(t);

	angband_term[i] = t;
}

const char help_stats[] = "Stats mode, subopts -r(andarts)";

/*
 * Usage:
 *
 * angband -mstats -- [-r] [-nNNNN]
 *
 *   -r      Turn on randarts
 *   -nNNNN  Make NNNN runs through the dungeon (default: 1)
 */

errr init_stats(int argc, char *argv[]) {
	int i;

	/* Skip over argv[0] */
	for (i = 1; i < argc; i++) {
		if (streq(argv[i], "-r")) {
			randarts = 1;
			continue;
		}
		if (prefix(argv[i], "-n")) {
			num_runs = atoi(&argv[i][2]);
			continue;
		}
		printf("init-stats: bad argument '%s'\n", argv[i]);
	}

	term_data_link(0);
	return 0;
}
