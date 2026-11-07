/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * model.c - the modem's settings from AT!M (dmodemctl.h)
 *
 * AT!M's lines, their fields apart by tabs:
 *
 *   model    1  <settings>
 *   setting  <key> <kind> <value> <default> <lo> <hi> <form> <group>
 *            <label> <help>
 *   choice   <value> <name> <label>        (a pick's, after it)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"
#include "util.h"

struct choice {
	char *value, *name, *label;
};

struct setting {
	char *key, *value, *def, *form, *group, *label, *help;
	int kind;
	long lo, hi;
	struct choice *choices;
	int n_choices;
};

struct dmc_model {
	struct setting *s;
	int n;
	char **groups;
	int n_groups;
	dmc_buf scratch;
	char why[96];
};

/* the line's fields: up to max, how many (the line cut in place) */
static int fields(char *line, char **f, int max)
{
	int n = 0;
	char *p = line;
	while (n < max) {
		f[n++] = p;
		char *t = strchr(p, '\t');
		if (!t)
			break;
		*t = '\0';
		p = t + 1;
	}
	return n;
}

static void add_group(dmc_model *m, const char *g)
{
	for (int i = 0; i < m->n_groups; i++)
		if (!strcmp(m->groups[i], g))
			return;
	char **p = realloc(m->groups, sizeof(*p) * (size_t)(m->n_groups + 1));
	if (!p)
		abort();
	m->groups = p;
	m->groups[m->n_groups++] = dup_str(g);
}

dmc_model *dmc_model_parse(const char *text)
{
	if (!text)
		return NULL;
	char *copy = dup_str(text);
	dmc_model *m = calloc(1, sizeof(*m));
	if (!m)
		abort();
	int seen_model = 0, declared = -1;
	char *save = copy;
	while (save) {
		char *line = save;
		char *nl = strchr(line, '\n');
		if (nl) {
			*nl = '\0';
			save = nl + 1;
		} else {
			save = NULL;
		}
		size_t len = strlen(line);
		if (len && line[len - 1] == '\r')
			line[len - 1] = '\0';
		char *f[12];
		int n = fields(line, f, 12);
		if (!strcmp(f[0], "model") && n >= 3) {
			if (atoi(f[1]) != 1)
				break;          /* a format we do not know */
			seen_model = 1;
			declared = atoi(f[2]);
		} else if (!strcmp(f[0], "setting") && n >= 11 && seen_model) {
			struct setting *p = realloc(m->s, sizeof(*p) *
						    (size_t)(m->n + 1));
			if (!p)
				abort();
			m->s = p;
			struct setting *s = &m->s[m->n++];
			memset(s, 0, sizeof(*s));
			s->key = dup_str(f[1]);
			s->kind = !strcmp(f[2], "pick") ? DMC_PICK :
				!strcmp(f[2], "number") ? DMC_NUMBER : DMC_TEXT;
			s->value = dup_str(f[3]);
			s->def = dup_str(f[4]);
			s->lo = strtol(f[5], NULL, 10);
			s->hi = strtol(f[6], NULL, 10);
			s->form = dup_str(f[7]);
			s->group = dup_str(f[8]);
			s->label = dup_str(f[9]);
			s->help = dup_str(f[10]);
			add_group(m, s->group);
		} else if (!strcmp(f[0], "choice") && n >= 4 && m->n) {
			struct setting *s = &m->s[m->n - 1];
			struct choice *c = realloc(s->choices, sizeof(*c) *
						   (size_t)(s->n_choices + 1));
			if (!c)
				abort();
			s->choices = c;
			c = &s->choices[s->n_choices++];
			c->value = dup_str(f[1]);
			c->name = dup_str(f[2]);
			c->label = dup_str(f[3]);
		}
	}
	free(copy);
	if (!seen_model || (declared >= 0 && declared != m->n)) {
		dmc_model_free(m);
		return NULL;
	}
	return m;
}

void dmc_model_free(dmc_model *m)
{
	if (!m)
		return;
	for (int i = 0; i < m->n; i++) {
		struct setting *s = &m->s[i];
		free(s->key);
		free(s->value);
		free(s->def);
		free(s->form);
		free(s->group);
		free(s->label);
		free(s->help);
		for (int j = 0; j < s->n_choices; j++) {
			free(s->choices[j].value);
			free(s->choices[j].name);
			free(s->choices[j].label);
		}
		free(s->choices);
	}
	free(m->s);
	for (int i = 0; i < m->n_groups; i++)
		free(m->groups[i]);
	free(m->groups);
	buf_free(&m->scratch);
	free(m);
}

int dmc_model_count(const dmc_model *m)
{
	return m ? m->n : 0;
}

static const struct setting *at(const dmc_model *m, int i)
{
	static const struct setting none = { "", "", "", "", "", "", "", 0,
					     0, 0, NULL, 0 };
	return m && i >= 0 && i < m->n ? &m->s[i] : &none;
}

const char *dmc_setting_key(const dmc_model *m, int i) { return at(m, i)->key; }
int dmc_setting_kind(const dmc_model *m, int i) { return at(m, i)->kind; }
const char *dmc_setting_value(const dmc_model *m, int i) { return at(m, i)->value; }
const char *dmc_setting_default(const dmc_model *m, int i) { return at(m, i)->def; }
long dmc_setting_lo(const dmc_model *m, int i) { return at(m, i)->lo; }
long dmc_setting_hi(const dmc_model *m, int i) { return at(m, i)->hi; }
const char *dmc_setting_form(const dmc_model *m, int i) { return at(m, i)->form; }
const char *dmc_setting_group(const dmc_model *m, int i) { return at(m, i)->group; }
const char *dmc_setting_label(const dmc_model *m, int i) { return at(m, i)->label; }
const char *dmc_setting_help(const dmc_model *m, int i) { return at(m, i)->help; }
int dmc_setting_choices(const dmc_model *m, int i) { return at(m, i)->n_choices; }

static const struct choice *choice_at(const dmc_model *m, int i, int c)
{
	static const struct choice none = { "", "", "" };
	const struct setting *s = at(m, i);
	return c >= 0 && c < s->n_choices ? &s->choices[c] : &none;
}

const char *dmc_choice_value(const dmc_model *m, int i, int c)
{
	return choice_at(m, i, c)->value;
}

const char *dmc_choice_name(const dmc_model *m, int i, int c)
{
	return choice_at(m, i, c)->name;
}

const char *dmc_choice_label(const dmc_model *m, int i, int c)
{
	return choice_at(m, i, c)->label;
}

int dmc_setting_choice_of(const dmc_model *m, int i, const char *v)
{
	const struct setting *s = at(m, i);
	for (int c = 0; c < s->n_choices; c++)
		if (same_ci(s->choices[c].value, v ? v : ""))
			return c;
	return -1;
}

int dmc_model_find(const dmc_model *m, const char *key)
{
	for (int i = 0; m && i < m->n; i++)
		if (!strcmp(m->s[i].key, key ? key : ""))
			return i;
	return -1;
}

int dmc_model_groups(const dmc_model *m)
{
	return m ? m->n_groups : 0;
}

const char *dmc_model_group(const dmc_model *m, int g)
{
	return m && g >= 0 && g < m->n_groups ? m->groups[g] : "";
}

/* the value a number: digits only, within lo and hi */
static int number_ok(const struct setting *s, const char *v, char *why,
		     size_t n)
{
	if (!*v) {
		snprintf(why, n, "Enter a number");
		return 0;
	}
	for (const char *p = v; *p; p++)
		if (*p < '0' || *p > '9') {
			snprintf(why, n, "Enter a number");
			return 0;
		}
	if (strlen(v) > 10) {
		snprintf(why, n, "From %ld to %ld", s->lo, s->hi);
		return 0;
	}
	long x = strtol(v, NULL, 10);
	if (x < s->lo || x > s->hi) {
		snprintf(why, n, "From %ld to %ld", s->lo, s->hi);
		return 0;
	}
	return 1;
}

const char *dmc_setting_check(const dmc_model *m, int i, const char *value)
{
	static char none[] = "No such setting";
	if (!m || i < 0 || i >= m->n)
		return none;
	char *why = ((dmc_model *)m)->why;
	const size_t size = sizeof(m->why);
	const struct setting *s = at(m, i);
	const char *v = value ? value : "";
	why[0] = '\0';
	switch (s->kind) {
	case DMC_PICK:
		if (dmc_setting_choice_of(m, i, v) < 0)
			snprintf(why, size, "Not one of the choices");
		break;
	case DMC_NUMBER:
		number_ok(s, v, why, size);
		break;
	default:
		/* a list of numbers, apart by commas (some may be left
		   out, as AT takes them) */
		if (!*v)
			snprintf(why, size, "Enter the values");
		for (const char *p = v; *p; p++)
			if ((*p < '0' || *p > '9') && *p != ',') {
				snprintf(why, size,
					 "Numbers apart by commas");
				break;
			}
		break;
	}
	return why;
}

const char *dmc_setting_command(const dmc_model *m, int i, const char *value)
{
	dmc_model *mm = (dmc_model *)m;
	if (!m)
		return "";
	buf_clear(&mm->scratch);
	if (*dmc_setting_check(m, i, value))
		return "";
	const struct setting *s = at(m, i);
	const char *v = value;
	/* (a pick takes its value as the modem lists it) */
	if (s->kind == DMC_PICK)
		v = s->choices[dmc_setting_choice_of(m, i, value)].value;
	buf_adds(&mm->scratch, "AT");
	for (const char *p = s->form; *p; p++) {
		if (p[0] == '%' && p[1]) {
			p++;
			if (*p == 's')
				buf_adds(&mm->scratch, v);
			else
				buf_addc(&mm->scratch, *p);
		} else {
			buf_addc(&mm->scratch, *p);
		}
	}
	return buf_str(&mm->scratch);
}

void dmc_setting_set(dmc_model *m, int i, const char *value)
{
	if (!m || i < 0 || i >= m->n)
		return;
	set_str(&m->s[i].value, value);
}
