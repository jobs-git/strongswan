/*
 * Copyright (C) 2010 Tobias Brunner
 * Copyright (C) 2008 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <glob.h>
#include <libgen.h>

#include "settings.h"

#include "debug.h"
#include "utils/linked_list.h"
#include "threading/rwlock.h"

#define MAX_INCLUSION_LEVEL		10

typedef struct private_settings_t private_settings_t;
typedef struct section_t section_t;
typedef struct kv_t kv_t;

/**
 * private data of settings
 */
struct private_settings_t {

	/**
	 * public functions
	 */
	settings_t public;

	/**
	 * top level section
	 */
	section_t *top;

	/**
	 * contents of loaded files
	 */
	linked_list_t *files;

	/**
	 * lock to safely access the settings
	 */
	rwlock_t *lock;
};

/**
 * section containing subsections and key value pairs
 */
struct section_t {

	/**
	 * name of the section
	 */
	char *name;

	/**
	 * subsections, as section_t
	 */
	linked_list_t *sections;

	/**
	 * key value pairs, as kv_t
	 */
	linked_list_t *kv;
};

/**
 * Key value pair
 */
struct kv_t {

	/**
	 * key string, relative
	 */
	char *key;

	/**
	 * value as string
	 */
	char *value;
};

/**
 * create a section with the given name
 */
static section_t *section_create(char *name)
{
	section_t *this;
	INIT(this,
		.name = name,
		.sections = linked_list_create(),
		.kv = linked_list_create(),
	);
	return this;
}

/**
 * destroy a section
 */
static void section_destroy(section_t *this)
{
	this->kv->destroy_function(this->kv, free);
	this->sections->destroy_function(this->sections, (void*)section_destroy);
	free(this);
}

/**
 * callback to find a section by name
 */
static bool section_find(section_t *this, char *name)
{
	return streq(this->name, name);
}

/**
 * callback to find a kv pair by key
 */
static bool kv_find(kv_t *this, char *key)
{
	return streq(this->key, key);
}

/**
 * Print a format key, but consume already processed arguments
 */
static bool print_key(char *buf, int len, char *start, char *key, va_list args)
{
	va_list copy;
	bool res;
	char *pos;

	va_copy(copy, args);
	while (start < key)
	{
		pos = strchr(start, '%');
		if (!pos)
		{
			start += strlen(start) + 1;
			continue;
		}
		pos++;
		switch (*pos)
		{
			case 'd':
				va_arg(copy, int);
				break;
			case 's':
				va_arg(copy, char*);
				break;
			case 'N':
				va_arg(copy, enum_name_t*);
				va_arg(copy, int);
				break;
			case '%':
				break;
			default:
				DBG1(DBG_CFG, "settings with %%%c not supported!", *pos);
				break;
		}
		start = pos;
		if (*start)
		{
			start++;
		}
	}
	res = vsnprintf(buf, len, key, copy) < len;
	va_end(copy);
	return res;
}

/**
 * find a section by a given key, using buffered key, reusable buffer
 */
static section_t *find_section_buffered(section_t *section,
					char *start, char *key, va_list args, char *buf, int len)
{
	char *pos;
	section_t *found = NULL;

	if (section == NULL)
	{
		return NULL;
	}
	pos = strchr(key, '.');
	if (pos)
	{
		*pos = '\0';
		pos++;
	}
	if (!print_key(buf, len, start, key, args))
	{
		return NULL;
	}
	if (section->sections->find_first(section->sections,
									  (linked_list_match_t)section_find,
									  (void**)&found, buf) == SUCCESS)
	{
		if (pos)
		{
			return find_section_buffered(found, start, pos, args, buf, len);
		}
	}
	return found;
}

/**
 * Find a section by a given key (thread-safe).
 */
static section_t *find_section(private_settings_t *this, section_t *section,
							   char *key, va_list args)
{
	char buf[128], keybuf[512];
	section_t *found;

	if (snprintf(keybuf, sizeof(keybuf), "%s", key) >= sizeof(keybuf))
	{
		return NULL;
	}
	this->lock->read_lock(this->lock);
	found = find_section_buffered(section, keybuf, keybuf, args, buf,
								  sizeof(buf));
	this->lock->unlock(this->lock);
	return found;
}

/**
 * Find the string value for a key, using buffered key, reusable buffer
 */
static char *find_value_buffered(section_t *section,
					char *start, char *key, va_list args, char *buf, int len)
{
	char *pos, *value = NULL;
	kv_t *kv;
	section_t *found = NULL;

	if (section == NULL)
	{
		return NULL;
	}

	pos = strchr(key, '.');
	if (pos)
	{
		*pos = '\0';
		pos++;

		if (!print_key(buf, len, start, key, args))
		{
			return NULL;
		}
		if (section->sections->find_first(section->sections,
										  (linked_list_match_t)section_find,
										  (void**)&found, buf) == SUCCESS)
		{
			return find_value_buffered(found, start, pos, args, buf, len);
		}
	}
	else
	{
		if (!print_key(buf, len, start, key, args))
		{
			return NULL;
		}
		if (section->kv->find_first(section->kv, (linked_list_match_t)kv_find,
									(void**)&kv, buf) == SUCCESS)
		{
			value = kv->value;
		}
	}
	return value;
}

/**
 * Find the string value for a key (thread-safe).
 */
static char *find_value(private_settings_t *this, section_t *section,
						char *key, va_list args)
{
	char buf[128], keybuf[512], *value;

	if (snprintf(keybuf, sizeof(keybuf), "%s", key) >= sizeof(keybuf))
	{
		return NULL;
	}
	this->lock->read_lock(this->lock);
	value = find_value_buffered(section, keybuf, keybuf, args, buf,
								sizeof(buf));
	this->lock->unlock(this->lock);
	return value;
}

METHOD(settings_t, get_str, char*,
	   private_settings_t *this, char *key, char *def, ...)
{
	char *value;
	va_list args;

	va_start(args, def);
	value = find_value(this, this->top, key, args);
	va_end(args);
	if (value)
	{
		return value;
	}
	return def;
}

METHOD(settings_t, get_bool, bool,
	   private_settings_t *this, char *key, bool def, ...)
{
	char *value;
	va_list args;

	va_start(args, def);
	value = find_value(this, this->top, key, args);
	va_end(args);
	if (value)
	{
		if (strcaseeq(value, "true") ||
			strcaseeq(value, "enabled") ||
			strcaseeq(value, "yes") ||
			strcaseeq(value, "1"))
		{
			return TRUE;
		}
		else if (strcaseeq(value, "false") ||
				 strcaseeq(value, "disabled") ||
				 strcaseeq(value, "no") ||
				 strcaseeq(value, "0"))
		{
			return FALSE;
		}
	}
	return def;
}

METHOD(settings_t, get_int, int,
	   private_settings_t *this, char *key, int def, ...)
{
	char *value;
	int intval;
	va_list args;

	va_start(args, def);
	value = find_value(this, this->top, key, args);
	va_end(args);
	if (value)
	{
		errno = 0;
		intval = strtol(value, NULL, 10);
		if (errno == 0)
		{
			return intval;
		}
	}
	return def;
}

METHOD(settings_t, get_double, double,
	   private_settings_t *this, char *key, double def, ...)
{
	char *value;
	double dval;
	va_list args;

	va_start(args, def);
	value = find_value(this, this->top, key, args);
	va_end(args);
	if (value)
	{
		errno = 0;
		dval = strtod(value, NULL);
		if (errno == 0)
		{
			return dval;
		}
	}
	return def;
}

METHOD(settings_t, get_time, u_int32_t,
	   private_settings_t *this, char *key, u_int32_t def, ...)
{
	char *value, *endptr;
	u_int32_t timeval;
	va_list args;

	va_start(args, def);
	value = find_value(this, this->top, key, args);
	va_end(args);
	if (value)
	{
		errno = 0;
		timeval = strtoul(value, &endptr, 10);
		if (errno == 0)
		{
			switch (*endptr)
			{
				case 'd':		/* time in days */
					timeval *= 24 * 3600;
					break;
				case 'h':		/* time in hours */
					timeval *= 3600;
					break;
				case 'm':		/* time in minutes */
					timeval *= 60;
					break;
				case 's':		/* time in seconds */
					default:
					break;
			}
			return timeval;
		}
	}
	return def;
}

/**
 * Enumerate section names, not sections
 */
static bool section_filter(void *null, section_t **in, char **out)
{
	*out = (*in)->name;
	return TRUE;
}

METHOD(settings_t, create_section_enumerator, enumerator_t*,
	   private_settings_t *this, char *key, ...)
{
	section_t *section;
	va_list args;

	va_start(args, key);
	section = find_section(this, this->top, key, args);
	va_end(args);

	if (!section)
	{
		return enumerator_create_empty();
	}
	this->lock->read_lock(this->lock);
	return enumerator_create_filter(
				section->sections->create_enumerator(section->sections),
				(void*)section_filter, this->lock, (void*)this->lock->unlock);
}

/**
 * Enumerate key and values, not kv_t entries
 */
static bool kv_filter(void *null, kv_t **in, char **key,
					  void *none, char **value)
{
	*key = (*in)->key;
	*value = (*in)->value;
	return TRUE;
}

METHOD(settings_t, create_key_value_enumerator, enumerator_t*,
	   private_settings_t *this, char *key, ...)
{
	section_t *section;
	va_list args;

	va_start(args, key);
	section = find_section(this, this->top, key, args);
	va_end(args);

	if (!section)
	{
		return enumerator_create_empty();
	}
	this->lock->read_lock(this->lock);
	return enumerator_create_filter(
					section->kv->create_enumerator(section->kv),
					(void*)kv_filter, this->lock, (void*)this->lock->unlock);
}

/**
 * parse text, truncate "skip" chars, delimited by term respecting brackets.
 *
 * Chars in "skip" are truncated at the beginning and the end of the resulting
 * token. "term" contains a list of characters to read up to (first match),
 * while "br" contains bracket counterparts found in "term" to skip.
 */
static char parse(char **text, char *skip, char *term, char *br, char **token)
{
	char *best = NULL;
	char best_term = '\0';

	/* skip leading chars */
	while (strchr(skip, **text))
	{
		(*text)++;
		if (!**text)
		{
			return 0;
		}
	}
	/* mark begin of subtext */
	*token = *text;
	while (*term)
	{
		char *pos = *text;
		int level = 1;

		/* find terminator */
		while (*pos)
		{
			if (*pos == *term)
			{
				level--;
			}
			else if (br && *pos == *br)
			{
				level++;
			}
			if (level == 0)
			{
				if (best == NULL || best > pos)
				{
					best = pos;
					best_term = *term;
				}
				break;
			}
			pos++;
		}
		/* try next terminator */
		term++;
		if (br)
		{
			br++;
		}
	}
	if (best)
	{
		/* update input */
		*text = best;
		/* null trailing bytes */
		do
		{
			*best = '\0';
			best--;
		}
		while (best >= *token && strchr(skip, *best));
		/* return found terminator */
		return best_term;
	}
	return 0;
}

/**
 * Check if "text" starts with "pattern".
 * Characters in "skip" are skipped first. If found, TRUE is returned and "text"
 * is modified to point to the character right after "pattern".
 */
static bool starts_with(char **text, char *skip, char *pattern)
{
	char *pos = *text;
	int len = strlen(pattern);
	while (strchr(skip, *pos))
	{
		pos++;
		if (!*pos)
		{
			return FALSE;
		}
	}
	if (strlen(pos) < len || !strneq(pos, pattern, len))
	{
		return FALSE;
	}
	*text = pos + len;
	return TRUE;
}

/**
 * Check if what follows in "text" is an include statement.
 * If this function returns TRUE, "text" will point to the character right after
 * the include pattern, which is returned in "pattern".
 */
static bool parse_include(char **text, char **pattern)
{
	char *pos = *text;
	if (!starts_with(&pos, "\n\t ", "include"))
	{
		return FALSE;
	}
	if (starts_with(&pos, "\t ", "="))
	{	/* ignore "include = value" */
		return FALSE;
	}
	*text = pos;
	return parse(text, "\t ", "\n", NULL, pattern) != 0;
}

/**
 * Forward declaration.
 */
static bool parse_files(linked_list_t *files, char *file, int level,
						char *pattern, section_t *section);

/**
 * Parse a section
 */
static bool parse_section(linked_list_t *files, char *file, int level,
						  char **text, section_t *section)
{
	bool finished = FALSE;
	char *key, *value, *inner;

	while (!finished)
	{
		if (parse_include(text, &value))
		{
			if (!parse_files(files, file, level, value, section))
			{
				DBG1(DBG_LIB, "failed to include '%s'", value);
				return FALSE;
			}
			continue;
		}
		switch (parse(text, "\t\n ", "{=#", NULL, &key))
		{
			case '{':
				if (parse(text, "\t ", "}", "{", &inner))
				{
					section_t *sub;
					if (!strlen(key))
					{
						DBG1(DBG_LIB, "skipping section without name in '%s'",
							 section->name);
						continue;
					}
					if (section->sections->find_first(section->sections,
											(linked_list_match_t)section_find,
											(void**)&sub, key) != SUCCESS)
					{
						sub = section_create(key);
						if (parse_section(files, file, level, &inner, sub))
						{
							section->sections->insert_last(section->sections,
														   sub);
							continue;
						}
						section_destroy(sub);
					}
					else
					{	/* extend the existing section */
						if (parse_section(files, file, level, &inner, sub))
						{
							continue;
						}
					}
					DBG1(DBG_LIB, "parsing subsection '%s' failed", key);
					break;
				}
				DBG1(DBG_LIB, "matching '}' not found near %s", *text);
				break;
			case '=':
				if (parse(text, "\t ", "\n", NULL, &value))
				{
					kv_t *kv;
					if (!strlen(key))
					{
						DBG1(DBG_LIB, "skipping value without key in '%s'",
							 section->name);
						continue;
					}
					if (section->kv->find_first(section->kv,
								(linked_list_match_t)kv_find,
								(void**)&kv, key) != SUCCESS)
					{
						INIT(kv,
							.key = key,
							.value = value,
						);
						section->kv->insert_last(section->kv, kv);
					}
					else
					{	/* replace with the most recently read value */
						kv->value = value;
					}
					continue;
				}
				DBG1(DBG_LIB, "parsing value failed near %s", *text);
				break;
			case '#':
				parse(text, "", "\n", NULL, &value);
				continue;
			default:
				finished = TRUE;
				continue;
		}
		return FALSE;
	}
	return TRUE;
}

/**
 * Parse a file and add the settings to the given section.
 */
static bool parse_file(linked_list_t *files, char *file, int level,
					   section_t *section)
{
	bool success;
	char *text, *pos;
	FILE *fd;
	int len;

	DBG2(DBG_LIB, "loading config file '%s'", file);
	fd = fopen(file, "r");
	if (fd == NULL)
	{
		DBG1(DBG_LIB, "'%s' does not exist or is not readable", file);
		return FALSE;
	}
	fseek(fd, 0, SEEK_END);
	len = ftell(fd);
	rewind(fd);
	text = malloc(len + 1);
	text[len] = '\0';
	if (fread(text, 1, len, fd) != len)
	{
		free(text);
		return FALSE;
	}
	fclose(fd);

	pos = text;
	success = parse_section(files, file, level, &pos, section);
	if (!success)
	{
		free(text);
	}
	else
	{
		files->insert_last(files, text);
	}
	return success;
}

/**
 * Load the files matching "pattern", which is resolved with glob(3).
 * If the pattern is relative, the directory of "file" is used as base.
 */
static bool parse_files(linked_list_t *files, char *file, int level,
						char *pattern, section_t *section)
{
	bool success = TRUE;
	int status;
	glob_t buf;
	char **expanded, pat[PATH_MAX];

	if (level > MAX_INCLUSION_LEVEL)
	{
		DBG1(DBG_LIB, "maximum level of %d includes reached, ignored",
			 MAX_INCLUSION_LEVEL);
		return TRUE;
	}

	if (!strlen(pattern))
	{
		DBG2(DBG_LIB, "empty include pattern, ignored");
		return TRUE;
	}

	if (!file || pattern[0] == '/')
	{	/* absolute path */
		if (snprintf(pat, sizeof(pat), "%s", pattern) >= sizeof(pat))
		{
			DBG1(DBG_LIB, "include pattern too long, ignored");
			return TRUE;
		}
	}
	else
	{	/* base relative paths to the directory of the current file */
		char *dir = strdup(file);
		dir = dirname(dir);
		if (snprintf(pat, sizeof(pat), "%s/%s", dir, pattern) >= sizeof(pat))
		{
			DBG1(DBG_LIB, "include pattern too long, ignored");
			free(dir);
			return TRUE;
		}
		free(dir);
	}
	status = glob(pat, GLOB_ERR, NULL, &buf);
	if (status == GLOB_NOMATCH)
	{
		DBG2(DBG_LIB, "no files found matching '%s', ignored", pat);
	}
	else if (status != 0)
	{
		DBG1(DBG_LIB, "expanding file pattern '%s' failed", pat);
		success = FALSE;
	}
	else
	{
		for (expanded = buf.gl_pathv; *expanded != NULL; expanded++)
		{
			success &= parse_file(files, *expanded, level + 1, section);
			if (!success)
			{
				break;
			}
		}
	}
	globfree(&buf);
	return success;
}

/**
 * Recursivly extends "base" with "extension".
 */
static void section_extend(section_t *base, section_t *extension)
{
	enumerator_t *enumerator;
	section_t *sec;
	kv_t *kv;

	enumerator = extension->sections->create_enumerator(extension->sections);
	while (enumerator->enumerate(enumerator, (void**)&sec))
	{
		section_t *found;
		if (base->sections->find_first(base->sections,
					(linked_list_match_t)section_find, (void**)&found,
					sec->name) == SUCCESS)
		{
			section_extend(found, sec);
		}
		else
		{
			extension->sections->remove_at(extension->sections, enumerator);
			base->sections->insert_last(base->sections, sec);
		}
	}
	enumerator->destroy(enumerator);

	enumerator = extension->kv->create_enumerator(extension->kv);
	while (enumerator->enumerate(enumerator, (void**)&kv))
	{
		kv_t *found;
		if (base->kv->find_first(base->kv, (linked_list_match_t)kv_find,
					(void**)&found, kv->key) == SUCCESS)
		{
			found->value = kv->value;
		}
		else
		{
			extension->kv->remove_at(extension->kv, enumerator);
			base->kv->insert_last(base->kv, kv);
		}
	}
	enumerator->destroy(enumerator);
}

/**
 * Load settings from files matching the given file pattern.
 * All sections and values are added relative to "parent".
 * All files (even included ones) have to be loaded successfully.
 */
static bool load_files_internal(private_settings_t *this, section_t *parent,
								char *pattern)
{
	char *text;
	linked_list_t *files = linked_list_create();
	section_t *section = section_create(NULL);

	if (!parse_files(files, NULL, 0, pattern, section))
	{
		files->destroy_function(files, (void*)free);
		section_destroy(section);
		return FALSE;
	}

	this->lock->write_lock(this->lock);
	/* extend parent section */
	section_extend(parent, section);
	/* move contents of loaded files to main store */
	while (files->remove_first(files, (void**)&text) == SUCCESS)
	{
		this->files->insert_last(this->files, text);
	}
	this->lock->unlock(this->lock);

	section_destroy(section);
	files->destroy(files);
	return TRUE;
}

METHOD(settings_t, load_files, bool,
	   private_settings_t *this, char *pattern)
{
	return load_files_internal(this, this->top, pattern);
}

METHOD(settings_t, load_files_section, bool,
	   private_settings_t *this, char *pattern, char *key, ...)
{
	section_t *section;
	va_list args;

	va_start(args, key);
	section = find_section(this, this->top, key, args);
	va_end(args);

	if (!section)
	{
		return FALSE;
	}
	return load_files_internal(this, section, pattern);
}

METHOD(settings_t, destroy, void,
	   private_settings_t *this)
{
	section_destroy(this->top);
	this->files->destroy_function(this->files, (void*)free);
	this->lock->destroy(this->lock);
	free(this);
}

/*
 * see header file
 */
settings_t *settings_create(char *file)
{
	private_settings_t *this;

	INIT(this,
		.public = {
			.get_str = _get_str,
			.get_int = _get_int,
			.get_double = _get_double,
			.get_time = _get_time,
			.get_bool = _get_bool,
			.create_section_enumerator = _create_section_enumerator,
			.create_key_value_enumerator = _create_key_value_enumerator,
			.load_files = _load_files,
			.load_files_section = _load_files_section,
			.destroy = _destroy,
		},
		.top = section_create(NULL),
		.files = linked_list_create(),
		.lock = rwlock_create(RWLOCK_TYPE_DEFAULT),
	);

	if (file == NULL)
	{
		file = STRONGSWAN_CONF;
	}

	load_files(this, file);

	return &this->public;
}

