/*
 * Tests for our filesystem portability code.
 */

#include "c.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lib/pg_tap.h"

/*
 * Make an absolute path under the tmp_check directory.
 */
static void
make_path(char *buffer, const char *name)
{
	const char *directory;

	directory = getenv("TESTDATADIR");
	PG_REQUIRE(directory);

	snprintf(buffer, MAXPGPATH, "%s/%s", directory, name);
}

/*
 * Tests that are expected to compile and pass on Unix and Windows.  On
 * Windows, many of these functions are wrappers in libpgport.
 */
static void
simple_tests(void)
{
	int fd;
	char path[MAXPGPATH];
	char path2[MAXPGPATH];
	char path3[MAXPGPATH];
	struct stat statbuf;
	ssize_t size;

	/* Set up test directory structure. */

	make_path(path, "dir1");
	PG_REQUIRE_SYS(mkdir(path, 0777) == 0);
	make_path(path, "dir1/dir2");
	PG_REQUIRE_SYS(mkdir(path, 0777) == 0);
	make_path(path, "dir1/test.txt");
	fd = open(path, O_CREAT | O_RDWR, 0777);
	PG_REQUIRE_SYS(fd >= 0);
	PG_REQUIRE_SYS(write(fd, "hello world\n", 12) == 12);
	PG_REQUIRE_SYS(close(fd) == 0);

	/* Tests for symlink()/readline(). */

	make_path(path, "dir999/dir3");	/* name of symlink to create */
	make_path(path2, "dir1/dir2");	/* name of directory it will point to */
	PG_EXPECT(symlink(path2, path) == -1, "symlink fails on missing parent") ;
	PG_EXPECT_EQ(errno, ENOENT, "got ENOENT");

	make_path(path, "dir1/dir3");	/* name of symlink to create */
	make_path(path2, "dir1/dir2");	/* name of directory it will point to */
	PG_EXPECT_SYS(symlink(path2, path) == 0, "create symlink");

	size = readlink(path, path3, sizeof(path3));
	PG_EXPECT_EQ(size, strlen(path2), "readlink reports expected size");
	PG_EXPECT(memcmp(path2, path3, size) == 0, "readlink reports expected target");

	PG_EXPECT(readlink("does-not-exist", path3, sizeof(path3)) == -1, "readlink fails on missing path") ;
	PG_EXPECT_EQ(errno, ENOENT, "got ENOENT");

	/* Tests for opendir(), readdir(), closedir(). */
	{
		DIR *dir;
		struct dirent *de;
		int dot = -1;
		int dotdot = -1;
		int dir2 = -1;
		int dir3 = -1;
		int test_txt = -1;

		make_path(path, "does-not-exist");
		PG_EXPECT(opendir(path) == NULL, "open missing directory fails");
		PG_EXPECT_EQ(errno, ENOENT, "got ENOENT");

		make_path(path, "dir1");
		PG_EXPECT_SYS((dir = opendir(path)), "open directory");

#ifdef DT_REG
/*
 * Linux, *BSD, macOS and our Windows wrappers have BSD d_type.  On a few rare
 * file systems, it may reported as DT_UNKNOWN so we have to tolerate that too.
 */
#define LOAD(name, variable) if (strcmp(de->d_name, name) == 0) variable = de->d_type
#define CHECK(name, variable, type) \
	PG_EXPECT(variable != -1, name " was found"); \
	PG_EXPECT(variable == DT_UNKNOWN || variable == type, name " has type DT_UNKNOWN or " #type)
#else
/*
 * Solaris, AIX and Cygwin do not have it (it's not in POSIX).  Just check that
 * we saw the file and ignore the type.
 */
#define LOAD(name, variable) if (strcmp(de->d_name, name) == 0) variable = 0
#define CHECK(name, variable, type) \
	PG_EXPECT(variable != -1, name " was found")
#endif

		/* Load and check in two phases because the order is unknown. */
		errno = 0;
		while ((de = readdir(dir)))
		{
			LOAD(".", dot);
			LOAD("..", dotdot);
			LOAD("dir2", dir2);
			LOAD("dir3", dir3);
			LOAD("test.txt", test_txt);
		}
		PG_EXPECT_SYS(errno == 0, "ran out of dirents without error");

		CHECK(".", dot, DT_DIR);
		CHECK("..", dotdot, DT_DIR);
		CHECK("dir2", dir2, DT_DIR);
		CHECK("dir3", dir3, DT_LNK);
		CHECK("test.txt", test_txt, DT_REG);
	}

#undef LOAD
#undef CHECK

	/* Tests for fstat(). */

	make_path(path, "dir1/test.txt");
	fd = open(path, O_RDWR, 0777);
	PG_REQUIRE_SYS(fd >= 0);
	memset(&statbuf, 0, sizeof(statbuf));
	PG_EXPECT(fstat(fd, &statbuf) == 0, "fstat regular file");
	PG_EXPECT(S_ISREG(statbuf.st_mode), "type is REG");
	PG_REQUIRE_SYS(close(fd) == 0);

	/* Tests for stat(). */

	PG_EXPECT(stat("does-not-exist.txt", &statbuf) == -1, "stat missing file fails");
	PG_EXPECT_EQ(errno, ENOENT, "got ENOENT");

	make_path(path, "dir1/test.txt");
	memset(&statbuf, 0, sizeof(statbuf));
	PG_EXPECT(stat(path, &statbuf) == 0, "stat regular file");
	PG_EXPECT(S_ISREG(statbuf.st_mode), "type is REG");
	PG_EXPECT(statbuf.st_size == 12, "has expected size");

	make_path(path, "dir1/dir2");
	memset(&statbuf, 0, sizeof(statbuf));
	PG_EXPECT(stat(path, &statbuf) == 0, "stat directory");
	PG_EXPECT(S_ISDIR(statbuf.st_mode), "type is DIR");

	make_path(path, "dir1/dir3");
	memset(&statbuf, 0, sizeof(statbuf));
	PG_EXPECT(stat(path, &statbuf) == 0, "stat symlink");
	PG_EXPECT(S_ISDIR(statbuf.st_mode), "type is DIR");

	/* Tests for lstat(). */

	PG_EXPECT(stat("does-not-exist.txt", &statbuf) == -1, "lstat missing file fails");
	PG_EXPECT_EQ(errno, ENOENT, "got ENOENT");

	make_path(path, "dir1/test.txt");
	memset(&statbuf, 0, sizeof(statbuf));
	PG_EXPECT(lstat(path, &statbuf) == 0, "lstat regular file");
	PG_EXPECT(S_ISREG(statbuf.st_mode), "type is REG");
	PG_EXPECT(statbuf.st_size == 12, "has expected size");

	make_path(path2, "dir1/dir2");
	memset(&statbuf, 0, sizeof(statbuf));
	PG_EXPECT(lstat(path2, &statbuf) == 0, "lstat directory");
	PG_EXPECT(S_ISDIR(statbuf.st_mode), "type is DIR");

	make_path(path, "dir1/dir3");
	memset(&statbuf, 0, sizeof(statbuf));
	PG_EXPECT(lstat(path, &statbuf) == 0, "lstat symlink");
	PG_EXPECT(S_ISLNK(statbuf.st_mode), "type is LNK");
	PG_EXPECT_EQ(statbuf.st_size, strlen(path2), "got expected symlink size");
}

#ifdef WIN32
/*
 * Tests that expect Windows-only behavior.
 */
static void
windows_tests(void)
{
}
#endif

int
main()
{
	PG_BEGIN_TESTS();

	simple_tests();
#ifdef WIN32
	windows_tests();
#endif

	PG_END_TESTS();
}
