/* vifm
 * Copyright (C) 2001 Ken Steen.
 * Copyright (C) 2011 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "filtering.h"

#include <assert.h> /* assert() */
#include <string.h> /* strdup() */

#include "cfg/config.h"
#include "ui/ui.h"
#include "utils/path.h"
#include "utils/str.h"
#include "utils/utils.h"
#include "filelist.h"

static void reset_filter(filter_t *filter);
static int is_newly_filtered(FileView *view, const dir_entry_t *entry,
		void *arg);
static int get_unfiltered_pos(const FileView *const view, int pos);
static int load_unfiltered_list(FileView *const view);
static void store_local_filter_position(FileView *const view, int pos);
static void update_filtering_lists(FileView *view, int add, int clear);
static void ensure_filtered_list_not_empty(FileView *view,
		dir_entry_t *parent_entry);
static int extract_previously_selected_pos(FileView *const view);
static void clear_local_filter_hist_after(FileView *const view, int pos);
static int find_nearest_neighour(const FileView *const view);
static void local_filter_finish(FileView *view);
static void append_slash(const char name[], char buf[], size_t buf_size);

void
filters_view_reset(FileView *view)
{
	view->invert = cfg.filter_inverted_by_default ? 1 : 0;
	view->prev_invert = view->invert;

	(void)replace_string(&view->prev_manual_filter, "");
	reset_filter(&view->manual_filter);
	(void)replace_string(&view->prev_auto_filter, "");
	reset_filter(&view->auto_filter);

	(void)replace_string(&view->local_filter.prev, "");
	reset_filter(&view->local_filter.filter);
	view->local_filter.in_progress = 0;
	view->local_filter.saved = NULL;
	view->local_filter.poshist = NULL;
	view->local_filter.poshist_len = 0U;
}

/* Resets filter to empty state (either initializes or clears it). */
static void
reset_filter(filter_t *filter)
{
	if(filter->raw == NULL)
	{
		(void)filter_init(filter, FILTER_DEF_CASE_SENSITIVITY);
	}
	else
	{
		filter_clear(filter);
	}
}

void
set_dot_files_visible(FileView *view, int visible)
{
	view->hide_dot = !visible;
	ui_view_schedule_reload(view);
}

void
toggle_dot_files(FileView *view)
{
	set_dot_files_visible(view, view->hide_dot);
}

void
filter_selected_files(FileView *view)
{
	dir_entry_t *entry;
	filter_t filter;
	int filtered;

	(void)filter_init(&filter, FILTER_DEF_CASE_SENSITIVITY);

	/* Traverse items and update/create filter values. */
	entry = NULL;
	while(iter_selection_or_current(view, &entry))
	{
		const char *name = entry->name;
		char name_with_slash[NAME_MAX + 1 + 1];

		if(is_directory_entry(entry))
		{
			append_slash(entry->name, name_with_slash, sizeof(name_with_slash));
			name = name_with_slash;
		}

		(void)filter_append(&view->auto_filter, name);
		(void)filter_append(&filter, name);
	}

	/* Update entry lists to remove entries that must be filtered out now.  No
	 * view reload is needed. */
	filtered = zap_entries(view, view->dir_entry, &view->list_rows,
			&is_newly_filtered, &filter, 0);
	if(flist_custom_active(view))
	{
		(void)zap_entries(view, view->custom.entries, &view->custom.entry_count,
				&is_newly_filtered, &filter, 1);
	}
	else
	{
		view->filtered += filtered;
	}

	filter_dispose(&filter);

	flist_ensure_pos_is_valid(view);
	ui_view_schedule_redraw(view);
}

/* zap_entries() filter to filter-out files that match filter passed in the
 * arg. */
static int
is_newly_filtered(FileView *view, const dir_entry_t *entry, void *arg)
{
	filter_t *const filter = arg;

	/* FIXME: some very long file names won't be matched against some
	 * regexps. */
	char name_with_slash[NAME_MAX + 1 + 1];
	const char *filename = entry->name;

	if(is_directory_entry(entry))
	{
		append_slash(filename, name_with_slash, sizeof(name_with_slash));
		filename = name_with_slash;
	}

	return filter_matches(filter, filename) == 0;
}

void
remove_filename_filter(FileView *view)
{
	if(filename_filter_is_empty(view))
	{
		return;
	}

	(void)replace_string(&view->prev_manual_filter, view->manual_filter.raw);
	(void)replace_string(&view->prev_auto_filter, view->auto_filter.raw);
	view->prev_invert = view->invert;

	filename_filter_clear(view);
	view->invert = cfg.filter_inverted_by_default ? 1 : 0;

	ui_view_schedule_full_reload(view);
}

int
filename_filter_is_empty(FileView *view)
{
	return filter_is_empty(&view->manual_filter)
	    && filter_is_empty(&view->auto_filter);
}

void
filename_filter_clear(FileView *view)
{
	filter_clear(&view->auto_filter);
	filter_clear(&view->manual_filter);
	view->invert = 1;
}

void
restore_filename_filter(FileView *view)
{
	(void)filter_set(&view->manual_filter, view->prev_manual_filter);
	(void)filter_set(&view->auto_filter, view->prev_auto_filter);
	view->invert = view->prev_invert;
	ui_view_schedule_full_reload(view);
}

void
toggle_filter_inversion(FileView *view)
{
	view->invert = !view->invert;
	load_dir_list(view, 1);
	flist_set_pos(view, 0);
}

int
file_is_visible(FileView *view, const char filename[], int is_dir)
{
	/* FIXME: some very long file names won't be matched against some regexps. */
	char name_with_slash[NAME_MAX + 1 + 1];
	if(is_dir)
	{
		append_slash(filename, name_with_slash, sizeof(name_with_slash));
		filename = name_with_slash;
	}

	if(filter_matches(&view->auto_filter, filename) > 0)
	{
		return 0;
	}

	if(filter_matches(&view->local_filter.filter, filename) == 0)
	{
		return 0;
	}

	if(filter_is_empty(&view->manual_filter))
	{
		return 1;
	}

	if(filter_matches(&view->manual_filter, filename) > 0)
	{
		return !view->invert;
	}
	else
	{
		return view->invert;
	}
}

void
filters_dir_updated(FileView *view)
{
	filter_clear(&view->local_filter.filter);
}

void
local_filter_set(FileView *view, const char filter[])
{
	const int current_file_pos = view->local_filter.in_progress
		? get_unfiltered_pos(view, view->list_pos)
		: load_unfiltered_list(view);

	if(current_file_pos >= 0)
	{
		store_local_filter_position(view, current_file_pos);
	}

	(void)filter_change(&view->local_filter.filter, filter,
			!regexp_should_ignore_case(filter));

	update_filtering_lists(view, 1, 0);
}

/* Gets position of an item in dir_entry list at position pos in the unfiltered
 * list.  Returns index on success, otherwise -1 is returned. */
static int
get_unfiltered_pos(const FileView *const view, int pos)
{
	const int count = (int)view->local_filter.unfiltered_count;
	const char *const filename = view->dir_entry[pos].name;
	while(pos < count)
	{
		/* Compare pointers, which are the same for same entry in two arrays. */
		if(view->local_filter.unfiltered[pos].name == filename)
		{
			return pos;
		}
		++pos;
	}
	return -1;
}

/* Loads full list of files into unfiltered list of the view.  Returns positon
 * of file under cursor in the unfiltered list. */
static int
load_unfiltered_list(FileView *const view)
{
	int current_file_pos = view->list_pos;

	view->local_filter.in_progress = 1;

	view->local_filter.saved = strdup(view->local_filter.filter.raw);

	if(view->filtered > 0)
	{
		char full_path[PATH_MAX];
		dir_entry_t *entry;

		get_current_full_path(view, sizeof(full_path), full_path);

		filter_clear(&view->local_filter.filter);
		populate_dir_list(view, 1);

		/* Resolve current file position in updated list. */
		entry = entry_from_path(view->dir_entry, view->list_rows, full_path);
		if(entry != NULL)
		{
			current_file_pos = entry_to_pos(view, entry);
		}

		if(current_file_pos >= view->list_rows)
		{
			current_file_pos = view->list_rows - 1;
		}
	}
	else
	{
		/* Save unfiltered (by local filter) list for further use. */
		replace_dir_entries(view, &view->custom.entries,
				&view->custom.entry_count, view->dir_entry, view->list_rows);
	}

	view->local_filter.unfiltered = view->dir_entry;
	view->local_filter.unfiltered_count = view->list_rows;
	view->local_filter.prefiltered_count = view->filtered;
	view->dir_entry = NULL;

	return current_file_pos;
}

/* Adds local filter position (in unfiltered list) to position history. */
static void
store_local_filter_position(FileView *const view, int pos)
{
	size_t *const len = &view->local_filter.poshist_len;
	int *const arr = realloc(view->local_filter.poshist, sizeof(int)*(*len + 1));
	if(arr != NULL)
	{
		view->local_filter.poshist = arr;
		arr[*len] = pos;
		++*len;
	}
}

/* Copies/moves elements of the unfiltered list into dir_entry list.  add
 * parameter controls whether entries matching filter are copied into dir_entry
 * list.  clear parameter controls whether entries not matching filter are
 * cleared in unfiltered list. */
static void
update_filtering_lists(FileView *view, int add, int clear)
{
	size_t i;
	size_t list_size = 0U;
	dir_entry_t *parent_entry = NULL;

	for(i = 0; i < view->local_filter.unfiltered_count; i++)
	{
		/* FIXME: some very long file names won't be matched against some
		 * regexps. */
		char name_with_slash[NAME_MAX + 1 + 1];

		dir_entry_t *const entry = &view->local_filter.unfiltered[i];
		const char *name = entry->name;

		if(is_parent_dir(name))
		{
			parent_entry = entry;
			if(add && cfg_parent_dir_is_visible(is_root_dir(view->curr_dir)))
			{
				(void)add_dir_entry(&view->dir_entry, &list_size, entry);
			}
			continue;
		}

		if(is_directory_entry(entry))
		{
			append_slash(name, name_with_slash, sizeof(name_with_slash));
			name = name_with_slash;
		}

		if(filter_matches(&view->local_filter.filter, name) != 0)
		{
			if(add)
			{
				(void)add_dir_entry(&view->dir_entry, &list_size, entry);
			}
		}
		else
		{
			if(clear)
			{
				free_dir_entry(view, entry);
			}
		}
	}

	if(add)
	{
		view->list_rows = list_size;
		view->filtered = view->local_filter.prefiltered_count
		               + view->local_filter.unfiltered_count - list_size;
		ensure_filtered_list_not_empty(view, parent_entry);
	}
}

/* Use parent_entry to make filtered list not empty, or create such entry (if
 * parent_entry is NULL) and put it to original list. */
static void
ensure_filtered_list_not_empty(FileView *view, dir_entry_t *parent_entry)
{
	if(view->list_rows != 0U)
	{
		return;
	}

	if(parent_entry == NULL)
	{
		add_parent_dir(view);
		if(view->list_rows > 0)
		{
			(void)add_dir_entry(&view->local_filter.unfiltered,
					&view->local_filter.unfiltered_count,
					&view->dir_entry[view->list_rows - 1]);
		}
	}
	else
	{
		size_t list_size = 0U;
		(void)add_dir_entry(&view->dir_entry, &list_size, parent_entry);
		view->list_rows = list_size;
	}
}

void
local_filter_update_view(FileView *view, int rel_pos)
{
	int pos = extract_previously_selected_pos(view);

	if(pos < 0)
	{
		pos = find_nearest_neighour(view);
	}

	if(pos >= 0)
	{
		if(pos == 0 && is_parent_dir(view->dir_entry[0].name) &&
				view->list_rows > 0)
		{
			pos++;
		}

		view->list_pos = pos;
		view->top_line = pos - rel_pos;
	}
}

/* Finds one of previously selected files and updates list of visited files
 * if needed.  Returns file position in current list of files or -1. */
static int
extract_previously_selected_pos(FileView *const view)
{
	size_t i;
	for(i = 0; i < view->local_filter.poshist_len; i++)
	{
		const int unfiltered_pos = view->local_filter.poshist[i];
		const char *const file = view->local_filter.unfiltered[unfiltered_pos].name;
		const int filtered_pos = find_file_pos_in_list(view, file);

		if(filtered_pos >= 0)
		{
			clear_local_filter_hist_after(view, i);
			return filtered_pos;
		}
	}
	return -1;
}

/* Clears all positions after the pos. */
static void
clear_local_filter_hist_after(FileView *const view, int pos)
{
	view->local_filter.poshist_len -= view->local_filter.poshist_len - (pos + 1);
}

/* Find nearest filtered neighbour.  Returns index of nearest unfiltered
 * neighbour of the entry initially pointed to by cursor. */
static int
find_nearest_neighour(const FileView *const view)
{
	const int count = view->local_filter.unfiltered_count;

	if(view->local_filter.poshist_len > 0U)
	{
		const int unfiltered_orig_pos = view->local_filter.poshist[0U];
		int i;
		for(i = unfiltered_orig_pos; i < count; i++)
		{
			const int filtered_pos = find_file_pos_in_list(view,
					view->local_filter.unfiltered[i].name);
			if(filtered_pos >= 0)
			{
				return filtered_pos;
			}
		}
	}

	assert(view->list_rows > 0 && "List of files is empty.");
	return view->list_rows - 1;
}

void
local_filter_accept(FileView *view)
{
	if(!view->local_filter.in_progress)
	{
		return;
	}

	update_filtering_lists(view, 0, 1);

	local_filter_finish(view);

	cfg_save_filter_history(view->local_filter.filter.raw);

	/* Some of previously selected files could be filtered out, update number of
	 * selected files. */
	recount_selected_files(view);
}

void
local_filter_apply(FileView *view, const char filter[])
{
	if(view->local_filter.in_progress)
	{
		assert(!view->local_filter.in_progress && "Wrong local filter applying.");
		return;
	}

	(void)filter_set(&view->local_filter.filter, filter);
	cfg_save_filter_history(view->local_filter.filter.raw);
}

void
local_filter_cancel(FileView *view)
{
	if(!view->local_filter.in_progress)
	{
		return;
	}

	(void)filter_set(&view->local_filter.filter, view->local_filter.saved);

	free(view->dir_entry);
	view->dir_entry = NULL;
	view->list_rows = 0;

	update_filtering_lists(view, 1, 1);
	local_filter_finish(view);
}

/* Finishes filtering process and frees associated resources. */
static void
local_filter_finish(FileView *view)
{
	free(view->local_filter.unfiltered);
	free(view->local_filter.saved);
	view->local_filter.in_progress = 0;

	free(view->local_filter.poshist);
	view->local_filter.poshist = NULL;
	view->local_filter.poshist_len = 0U;
}

void
local_filter_remove(FileView *view)
{
	(void)replace_string(&view->local_filter.prev, view->local_filter.filter.raw);
	filter_clear(&view->local_filter.filter);
}

void
local_filter_restore(FileView *view)
{
	(void)filter_set(&view->local_filter.filter, view->local_filter.prev);
	(void)replace_string(&view->local_filter.prev, "");
}

int
local_filter_matches(FileView *view, const dir_entry_t *entry)
{
	/* FIXME: some very long file names won't be matched against some
	 * regexps. */
	char name_with_slash[NAME_MAX + 1 + 1];
	const char *filename = entry->name;
	if(is_directory_entry(entry))
	{
		append_slash(filename, name_with_slash, sizeof(name_with_slash));
		filename = name_with_slash;
	}

	return filter_matches(&view->local_filter.filter, filename) != 0;
}

/* Appends slash to the name and stores result in the buffer. */
static void
append_slash(const char name[], char buf[], size_t buf_size)
{
	const size_t nchars = copy_str(buf, buf_size - 1, name);
	buf[nchars - 1] = '/';
	buf[nchars] = '\0';
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 : */
