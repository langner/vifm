#include <stic.h>

#include <unistd.h> /* F_OK access() */

#include <stdio.h> /* remove() */

#include "../../src/io/iop.h"
#include "../../src/utils/utils.h"

static int not_windows(void);

static const char *const ORIG_FILE_NAME = "file";
static const char *const NEW_ORIG_FILE_NAME = "new_file";
static const char *const LINK_NAME = "link";

static void
create_file(const char path[])
{
	assert_int_equal(-1, access(path, F_OK));

	io_args_t args =
	{
		.arg1.path = path,
	};
	assert_int_equal(0, iop_mkfile(&args));

	assert_int_equal(0, access(path, F_OK));
}

SETUP()
{
	create_file(ORIG_FILE_NAME);
}

TEARDOWN()
{
	assert_int_equal(0, access(ORIG_FILE_NAME, F_OK));
	assert_int_equal(0, remove(ORIG_FILE_NAME));
	assert_int_equal(-1, access(ORIG_FILE_NAME, F_OK));
}

TEST(existent_file_is_not_overwritten_if_not_requested)
{
	assert_int_equal(-1, access(LINK_NAME, F_OK));

	{
		io_args_t args =
		{
			.arg1.path = LINK_NAME,
		};
		assert_int_equal(0, iop_mkfile(&args));
	}

	assert_int_equal(0, access(LINK_NAME, F_OK));

	io_args_t args =
	{
		.arg1.path = ORIG_FILE_NAME,
		.arg2.target = LINK_NAME,
	};
	assert_false(iop_ln(&args) == 0);

	assert_int_equal(0, remove(LINK_NAME));

	assert_int_equal(-1, access(LINK_NAME, F_OK));
}

TEST(existent_non_symlink_is_not_overwritten)
{
	create_file(LINK_NAME);

	io_args_t args =
	{
		.arg1.path = ORIG_FILE_NAME,
		.arg2.target = LINK_NAME,
		.arg3.crs = IO_CRS_REPLACE_FILES,
	};
	assert_int_equal(-1, iop_ln(&args));

	assert_int_equal(0, remove(LINK_NAME));
	assert_int_equal(-1, access(LINK_NAME, F_OK));
}

/* Creating symbolic links on Windows requires administrator rights. */
TEST(nonexistent_symlink_is_created, IF(not_windows))
{
	assert_int_equal(-1, access(LINK_NAME, F_OK));

	io_args_t args =
	{
		.arg1.path = ORIG_FILE_NAME,
		.arg2.target = LINK_NAME,
	};
	assert_int_equal(0, iop_ln(&args));

	assert_int_equal(0, access(LINK_NAME, F_OK));

	assert_int_equal(0, remove(LINK_NAME));

	assert_int_equal(-1, access(LINK_NAME, F_OK));
}

/* Creating symbolic links on Windows requires administrator rights. */
TEST(existent_symlink_is_changed, IF(not_windows))
{
	assert_int_equal(-1, access(LINK_NAME, F_OK));

	io_args_t args =
	{
		.arg1.path = ORIG_FILE_NAME,
		.arg2.target = LINK_NAME,
	};
	assert_int_equal(0, iop_ln(&args));

	assert_int_equal(0, access(LINK_NAME, F_OK));

	create_file(NEW_ORIG_FILE_NAME);

	args.arg1.path = NEW_ORIG_FILE_NAME;
	args.arg3.crs = IO_CRS_REPLACE_FILES;
	assert_int_equal(0, iop_ln(&args));

	assert_int_equal(0, remove(LINK_NAME));
	assert_int_equal(-1, access(LINK_NAME, F_OK));

	assert_int_equal(0, remove(NEW_ORIG_FILE_NAME));
	assert_int_equal(-1, access(NEW_ORIG_FILE_NAME, F_OK));
}

static int
not_windows(void)
{
	return get_env_type() != ET_WIN;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 : */
