/*
 * Tests for opt_check_destinations_differ() in
 * host/ubertooth-tools/src/opt_parse.c: two capture options given one file.
 *
 * Every writer opens its destination for writing, which truncates whatever
 * another has put there, so a run given one name twice has to be refused
 * before any of them is opened. These cases pin which pairs are one file,
 * which are not, and the line the user is told.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "opt_parse.h"

static int checks;
static int failures;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

/* The working directory every relative name below is written in, so a name
 * that exists was made by this test and a name that does not cannot be. */
static char work[] = "/tmp/ubertooth-destinations-XXXXXX";

/* What a pair of destinations is refused with, captured from standard error,
 * and the helper's own answer in *status. */
static void refusal_for(const opt_capture_dest *destinations, size_t count,
                        int *status, char *out, size_t size)
{
	FILE *capture = tmpfile();
	int saved = dup(fileno(stderr));
	long length;

	out[0] = '\0';
	*status = 0;
	if (capture == NULL || saved < 0) {
		if (capture != NULL)
			fclose(capture);
		if (saved >= 0)
			close(saved);
		CHECK(0);
		return;
	}

	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	*status = opt_check_destinations_differ(destinations, count);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);

	length = ftell(capture);
	rewind(capture);
	if (length > 0 && (size_t)length < size)
		out[fread(out, 1, (size_t)length, capture)] = '\0';
	fclose(capture);
}

/* A pair is one file: refused, with both options and the name in the line. */
static void check_refused(const char *first, const char *second,
                          const char *expected)
{
	const opt_capture_dest destinations[] = {
		{ "-r", NULL }, { "-q", first }, { "-d", second },
	};
	char line[256];
	int status;

	refusal_for(destinations, 3, &status, line, sizeof(line));
	CHECK(status == -1);
	if (strcmp(line, expected) != 0) {
		fprintf(stderr, "FAIL -q %s -d %s\n  wanted %s  got    %s",
		        first, second, expected, line[0] != '\0' ? line : "(nothing)\n");
		failures++;
	}
	checks++;
}

/* A pair is two files, or is not this check's to refuse: nothing said. */
static void check_accepted(const char *first, const char *second)
{
	const opt_capture_dest destinations[] = {
		{ "-r", NULL }, { "-q", first }, { "-d", second },
	};
	char line[256];
	int status;

	refusal_for(destinations, 3, &status, line, sizeof(line));
	CHECK(status == 0);
	if (line[0] != '\0') {
		fprintf(stderr, "FAIL -q %s -d %s: said %s", first, second, line);
		failures++;
	}
	checks++;
}

static void make_file(const char *name, const char *contents)
{
	FILE *file = fopen(name, "w");

	if (file == NULL) {
		fprintf(stderr, "FAIL cannot create %s: %s\n", name,
		        strerror(errno));
		failures++;
		return;
	}
	fputs(contents, file);
	fclose(file);
}

/* One name twice, which is the mistake the refusal is for, and the spellings
 * of one name that carry nothing extra. None of these files exists, so the
 * text is all there is to compare. */
static void test_one_name_written_twice(void)
{
	check_refused("cap.bin", "cap.bin",
	              "error: -q and -d cannot both write to cap.bin\n");
	check_refused("cap.bin", "./cap.bin",
	              "error: -q and -d cannot both write to one file"
	              " (cap.bin and ./cap.bin)\n");
	check_refused("./sub/cap.bin", "sub//./cap.bin",
	              "error: -q and -d cannot both write to one file"
	              " (./sub/cap.bin and sub//./cap.bin)\n");
	check_refused("/tmp/cap.bin", "/tmp/./cap.bin",
	              "error: -q and -d cannot both write to one file"
	              " (/tmp/cap.bin and /tmp/./cap.bin)\n");
}

/* Two names, so two files. */
static void test_two_names_are_two_files(void)
{
	check_accepted("cap.bin", "dump.bin");
	check_accepted("cap.bin", "cap.bin.1");
	check_accepted("cap.bin", "/tmp/cap.bin");
	check_accepted("sub/cap.bin", "cap.bin");
	/* A name that ends inside another asks for something within a file,
	 * which is not a destination at all: left to differ rather than
	 * claimed as the same file. */
	check_accepted("cap.bin", "cap.bin/.");
	check_accepted("cap.bin", "cap.bin/");
}

/* The directory holding the name is looked up, so the ".." elements and the
 * symbolic links above it are reduced by the system rather than guessed at,
 * and a relative name and its absolute path are one destination. */
static void test_one_entry_of_one_directory(void)
{
	char absolute[PATH_MAX];

	check_refused("sub/../cap.bin", "cap.bin",
	              "error: -q and -d cannot both write to one file"
	              " (sub/../cap.bin and cap.bin)\n");

	snprintf(absolute, sizeof(absolute), "%s/cap.bin", work);
	{
		char expected[PATH_MAX + 128];

		snprintf(expected, sizeof(expected),
		         "error: -q and -d cannot both write to one file"
		         " (cap.bin and %s)\n", absolute);
		check_refused("cap.bin", absolute, expected);
	}

	/* A directory that does not exist cannot be looked up, so two
	 * spellings of a name inside one are not compared. */
	check_accepted("gone/../cap.bin", "cap.bin");
}

/* An option that was not given is not a destination, and one destination
 * cannot clash with itself. */
static void test_nothing_to_compare(void)
{
	const opt_capture_dest one[] = { { "-d", "cap.bin" } };
	const opt_capture_dest none[] = {
		{ "-r", NULL }, { "-q", NULL }, { "-d", NULL },
	};
	char line[256];
	int status;

	refusal_for(one, 1, &status, line, sizeof(line));
	CHECK(status == 0 && line[0] == '\0');
	refusal_for(none, 3, &status, line, sizeof(line));
	CHECK(status == 0 && line[0] == '\0');
	refusal_for(NULL, 3, &status, line, sizeof(line));
	CHECK(status == 0 && line[0] == '\0');
}

/* A writer writes through a symbolic link, so a link and what it points at
 * are one destination. A hard link is one file on both names. */
static void test_links_to_one_file(void)
{
	make_file("real.bin", "the capture\n");

	if (symlink("real.bin", "link.bin") == 0)
		check_refused("real.bin", "link.bin",
		              "error: -q and -d cannot both write to one file"
		              " (real.bin and link.bin)\n");
	else
		printf("skipping the symbolic link case: %s\n", strerror(errno));

	if (link("real.bin", "hard.bin") == 0)
		check_refused("real.bin", "hard.bin",
		              "error: -q and -d cannot both write to one file"
		              " (real.bin and hard.bin)\n");
	else
		printf("skipping the hard link case: %s\n", strerror(errno));

	/* A second real file is still a second file. */
	make_file("other.bin", "another capture\n");
	check_accepted("real.bin", "other.bin");
}

/* A link to a capture that does not exist yet is the case neither name alone
 * nor a file comparison can see: the writer creates what the link points at,
 * so both writers would create one file. */
static void test_a_link_to_a_file_not_there_yet(void)
{
	if (symlink("ahead.bin", "dangling.bin") != 0) {
		printf("skipping the dangling link cases: %s\n",
		       strerror(errno));
		return;
	}
	check_refused("ahead.bin", "dangling.bin",
	              "error: -q and -d cannot both write to one file"
	              " (ahead.bin and dangling.bin)\n");
	check_refused("./ahead.bin", "dangling.bin",
	              "error: -q and -d cannot both write to one file"
	              " (./ahead.bin and dangling.bin)\n");
	/* It still points at one name only. */
	check_accepted("elsewhere.bin", "dangling.bin");

	/* A relative target is resolved against the link's own directory, not
	 * the working directory. */
	if (symlink("inside.bin", "sub/pointer.bin") != 0) {
		printf("skipping the link in a directory: %s\n",
		       strerror(errno));
		return;
	}
	check_refused("sub/inside.bin", "sub/pointer.bin",
	              "error: -q and -d cannot both write to one file"
	              " (sub/inside.bin and sub/pointer.bin)\n");
	check_accepted("inside.bin", "sub/pointer.bin");

	/* A target that climbs back out of the link's directory. The text of
	 * the resolved name carries "..", which only the directory lookup
	 * reduces. */
	if (symlink("../upward.bin", "sub/updown.bin") != 0) {
		printf("skipping the link out of a directory: %s\n",
		       strerror(errno));
		return;
	}
	check_refused("upward.bin", "sub/updown.bin",
	              "error: -q and -d cannot both write to one file"
	              " (upward.bin and sub/updown.bin)\n");
	check_accepted("sub/upward.bin", "sub/updown.bin");
}

/* Only a destination a writer would overwrite is refused. The null device
 * keeps nothing, so two captures discarding themselves there is a command
 * line that does what it says; a terminal is not that, because two streams
 * interleaved on one are neither capture. */
static void test_what_a_writer_would_not_overwrite(void)
{
	struct stat info;

	check_accepted("/dev/null", "/dev/null");
	check_accepted("/dev/null", "/dev/./null");
	/* A symbolic link to it is the same destination, in either order. */
	if (symlink("/dev/null", "sink.bin") == 0) {
		check_accepted("/dev/null", "sink.bin");
		check_accepted("sink.bin", "/dev/null");
	} else {
		printf("skipping the link to the null device: %s\n",
		       strerror(errno));
	}

	/* A terminal keeps what is written to it, as far as a reader is
	 * concerned, so one named twice is refused like a file. */
	if (stat("/dev/tty", &info) == 0)
		check_refused("/dev/tty", "/dev/tty",
		              "error: -q and -d cannot both write to"
		              " /dev/tty\n");
	else
		printf("skipping the terminal case: %s\n", strerror(errno));

	/* A directory is no destination at all; the writer's own open says so
	 * better than a line about a file would. */
	check_accepted("sub", "sub");
	check_accepted("./sub", "sub");
}

/* Build <links> symbolic links named "<prefix>-1" .. "<prefix>-<links>", each
 * pointing at the next, with the last pointing at "chain-end.bin", which is
 * left absent so the chain dangles. So "<prefix>-1" is <links> hops from the
 * name a writer would create. */
static void chain_of_links(int links, const char *prefix)
{
	char here[64], next[64];
	int i;

	unlink("chain-end.bin");
	for (i = links; i >= 1; i--) {
		snprintf(here, sizeof(here), "%s-%d", prefix, i);
		if (i == links)
			snprintf(next, sizeof(next), "chain-end.bin");
		else
			snprintf(next, sizeof(next), "%s-%d", prefix, i + 1);
		unlink(here);
		if (symlink(next, here) != 0) {
			printf("skipping part of the chain bound case: %s\n",
			       strerror(errno));
			return;
		}
	}
}

/* A chain of symbolic links is followed to its end, because the writer's open
 * does. A chain ending at a file that is not there yet loses a capture exactly
 * as a single link would, so reading only the first link is not enough. */
static void test_a_chain_of_links(void)
{
	unlink("chain-1");
	unlink("chain-2");
	unlink("chain-ghost.bin");
	if (symlink("chain-ghost.bin", "chain-2") != 0 ||
	    symlink("chain-2", "chain-1") != 0) {
		printf("skipping the chain case: %s\n", strerror(errno));
		return;
	}
	/* Two links deep, ending at a name that does not exist: both writers
	 * would create and truncate that one file. */
	check_refused("chain-ghost.bin", "chain-1",
	              "error: -q and -d cannot both write to one file"
	              " (chain-ghost.bin and chain-1)\n");
	check_refused("chain-1", "chain-ghost.bin",
	              "error: -q and -d cannot both write to one file"
	              " (chain-1 and chain-ghost.bin)\n");
	/* The middle of the chain names the same end, so the two routes to it
	 * are one destination. */
	check_refused("chain-1", "chain-2",
	              "error: -q and -d cannot both write to one file"
	              " (chain-1 and chain-2)\n");
	check_accepted("chain-1", "other.bin");

	/* A link pointing at itself must not spin: the walk is bounded. */
	unlink("loop");
	if (symlink("loop", "loop") == 0)
		check_accepted("loop", "other.bin");

	/* A chain that leaves the directory it started in. Each target is read
	 * against the directory of the link holding it, not the one the user
	 * named, so "sub/out" pointing at "../deep.bin" is this directory's. */
	unlink("sub/out");
	unlink("into-sub");
	unlink("deep.bin");
	if (symlink("../deep.bin", "sub/out") == 0 &&
	    symlink("sub/out", "into-sub") == 0)
		check_refused("deep.bin", "into-sub",
		              "error: -q and -d cannot both write to one file"
		              " (deep.bin and into-sub)\n");

	/* The declared bound. A dangling chain within reach is caught; one
	 * link longer is past what the walk resolves, and is the documented
	 * limit rather than a wrong answer: the name is compared as given. */
	chain_of_links(16, "hop16");
	check_refused("chain-end.bin", "hop16-1",
	              "error: -q and -d cannot both write to one file"
	              " (chain-end.bin and hop16-1)\n");
	chain_of_links(17, "hop17");
	check_accepted("chain-end.bin", "hop17-1");
}

/* A capture on standard output is a destination like any other once the shell
 * has pointed the stream at a file: the descriptor says which file that is. */
static void test_standard_output_pointed_at_a_file(void)
{
	int saved = dup(STDOUT_FILENO);
	int target = open("redirected.bin", O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (saved < 0 || target < 0) {
		printf("skipping the redirected standard output case: %s\n",
		       strerror(errno));
		if (saved >= 0)
			close(saved);
		if (target >= 0)
			close(target);
		return;
	}
	fflush(stdout);
	dup2(target, STDOUT_FILENO);
	close(target);

	check_refused("-", "redirected.bin",
	              "error: -q and -d cannot both write to one file"
	              " (standard output and redirected.bin)\n");
	check_accepted("-", "elsewhere.bin");
	/* A link pointing at the file named "-" is that file, not this stream,
	 * however its target reads once it has been resolved. The stream is on
	 * another file here, so the two are two destinations. */
	check_accepted("points-at-dash", "redirected.bin");
	check_accepted("redirected.bin", "points-at-dash");

	/* A link pointing at a path that names descriptor 1 does reach this
	 * stream, so it is the same destination as the file the stream is on.
	 * The target is recognized by its text: looking it up answers with a
	 * device of its own on the BSDs, where the pair would be missed. */
	unlink("points-at-stream");
	if (symlink("/dev/stdout", "points-at-stream") == 0) {
		check_refused("points-at-stream", "redirected.bin",
		              "error: -q and -d cannot both write to one file"
		              " (points-at-stream and redirected.bin)\n");
		check_refused("redirected.bin", "points-at-stream",
		              "error: -q and -d cannot both write to one file"
		              " (redirected.bin and points-at-stream)\n");
		check_accepted("points-at-stream", "elsewhere.bin");
		/* The same link beside a "-" the user wrote. Both reach this
		 * stream, so the pair is the one-capture-on-standard-output
		 * rule's, which each tool states in its own words, and this
		 * check leaves it alone rather than naming the file the stream
		 * happens to be on. */
		check_accepted("-", "points-at-stream");
		check_accepted("points-at-stream", "-");
	} else {
		printf("skipping the link-to-the-stream case: %s\n",
		       strerror(errno));
	}

	/* A symbolic link that happens to be NAMED "-". The stream is not a
	 * path, so it is not resolved through one: were "-" handed to a path
	 * operation, this link's target would take the stream's place, and
	 * then a pair of the stream and an ordinary file would read as one
	 * file while two captures of one file would read as two.
	 *
	 * The link is made last, since it changes what the name "-" resolves
	 * to for anything that does follow it. */
	unlink("-");
	close(creat("elsewhere.bin", 0600));
	if (symlink("elsewhere.bin", "-") == 0) {
		/* The stream beside the file that link points at. The stream
		 * is on redirected.bin, so these are two destinations -- but
		 * reading "-" as a path makes both of them elsewhere.bin. */
		check_accepted("-", "elsewhere.bin");
		check_accepted("elsewhere.bin", "-");
		/* The file named "-", written "./-", IS that link, so it and
		 * its target are one file. */
		check_refused("./-", "elsewhere.bin",
		              "error: -q and -d cannot both write to one file"
		              " (./- and elsewhere.bin)\n");
	} else {
		printf("skipping the link-named-dash case: %s\n",
		       strerror(errno));
	}
	unlink("-");
	/* The same link, dangling. This is the one that lost data: the stream
	 * took the missing target's place, so it matched nothing and a pair of
	 * the stream and the file it was pointed at went unrefused. */
	if (symlink("ghost.bin", "-") == 0) {
		check_refused("-", "redirected.bin",
		              "error: -q and -d cannot both write to one file"
		              " (standard output and redirected.bin)\n");
	} else {
		printf("skipping the dangling-link-named-dash case: %s\n",
		       strerror(errno));
	}
	unlink("-");

	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
}

/* An empty value names no destination; the writer's own open reports it, and
 * a pair of them is not this check's to call one file. */
static void test_an_empty_value(void)
{
	check_accepted("", "");
	check_accepted("", "cap.bin");
	check_accepted("cap.bin", "");
}

/* Two captures that both ask for standard output are the
 * one-capture-on-standard-output rule's, whatever they are called, so this
 * check leaves such a pair alone rather than refusing it in words of its own.
 * One of them beside a file name that is not that stream is two destinations
 * as usual. */
static void test_standard_output_is_left_alone(void)
{
	static const char *const stream[] = {
		"-", "/dev/stdout", "/dev/./stdout", "/dev/fd/1", "//dev//fd//1",
	};
	size_t i, j;

	for (i = 0; i < sizeof(stream) / sizeof(stream[0]); i++) {
		check_accepted(stream[i], "cap.bin");
		check_accepted("cap.bin", stream[i]);
		for (j = 0; j < sizeof(stream) / sizeof(stream[0]); j++)
			check_accepted(stream[i], stream[j]);
	}
	/* "./-" is the file named "-", so two of those are one file. */
	check_refused("./-", "./-",
	              "error: -q and -d cannot both write to ./-\n");
}

/* The stream beside the file named "-". Their texts reduce to each other, so a
 * path comparison calls them one destination, and they are two: one capture
 * goes to the stream and the other creates a file. Which file the stream is on
 * can only come from the descriptor, so that is the only thing asked. */
static void test_the_stream_beside_the_file_called_dash(void)
{
	int saved, target;

	check_accepted("-", "./-");
	check_accepted("./-", "-");
	check_accepted("-", ".///-");
	/* A symbolic link pointing at that file is the file too, not the
	 * stream, however its target reads. */
	CHECK(close(creat("-", 0600)) == 0);
	unlink("points-at-dash");
	CHECK(symlink("-", "points-at-dash") == 0);
	check_accepted("points-at-dash", "-");
	check_accepted("-", "points-at-dash");

	/* Pointed at that very file, though, the stream is that file, and the
	 * descriptor says so where no comparison of the text could. */
	saved = dup(STDOUT_FILENO);
	target = open("-", O_WRONLY, 0600);
	if (saved < 0 || target < 0) {
		printf("skipping the stream-on-that-file case: %s\n",
		       strerror(errno));
		if (saved >= 0)
			close(saved);
		if (target >= 0)
			close(target);
		return;
	}
	fflush(stdout);
	dup2(target, STDOUT_FILENO);
	close(target);

	check_refused("-", "./-",
	              "error: -q and -d cannot both write to one file"
	              " (standard output and ./-)\n");
	check_refused("points-at-dash", "-",
	              "error: -q and -d cannot both write to one file"
	              " (points-at-dash and standard output)\n");

	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
}

/* A destination reached through a descriptor the calling shell opened. The
 * name says nothing about which file that is and looking it up answers
 * differently on each system, so the descriptor itself is asked: it is
 * already open here, which makes the answer the same everywhere. */
static void test_descriptor_paths(void)
{
	char path[64];
	int held;

	/* Nothing is open on this descriptor, so the path names nothing and
	 * cannot be compared with anything. */
	check_accepted("cap.bin", "/dev/fd/9");
	/* A last element that is not a number at all is a file name. */
	check_accepted("cap.bin", "/dev/fd/1x");

	make_file("held.bin", "the capture\n");
	held = open("held.bin", O_WRONLY);
	if (held < 0) {
		printf("skipping the open descriptor case: %s\n",
		       strerror(errno));
		return;
	}
	snprintf(path, sizeof(path), "/dev/fd/%d", held);
	{
		char expected[PATH_MAX + 128];

		snprintf(expected, sizeof(expected),
		         "error: -q and -d cannot both write to one file"
		         " (held.bin and %s)\n", path);
		check_refused("held.bin", path, expected);
		snprintf(expected, sizeof(expected),
		         "error: -q and -d cannot both write to one file"
		         " (%s and held.bin)\n", path);
		check_refused(path, "held.bin", expected);
		/* A link to that descriptor path reaches the same file, and a
		 * writer follows the link. */
		unlink("to-held");
		if (symlink(path, "to-held") == 0)
			check_refused("to-held", "held.bin",
			              "error: -q and -d cannot both write to one"
			              " file (to-held and held.bin)\n");
		else
			printf("skipping the link-to-descriptor case: %s\n",
			       strerror(errno));
	}
	/* A ".." or "." element above the number is reduced by the lookup of
	 * the directory, so a destination spelled that way is the same
	 * descriptor wherever the writer's own open would reach it. The text
	 * comparisons in opt_parse.c leave ".." alone on purpose; this is the
	 * one place a lookup answers it. */
	{
		const char *shapes[] = { "/dev/fd/./%d", "/dev//fd/%d",
		                         "/dev/fd/../fd/%d" };
		char spelled[64], expected[PATH_MAX + 128];
		struct stat by_spelling, by_path;
		size_t i;

		for (i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
			snprintf(spelled, sizeof(spelled), shapes[i], held);
			if (stat(spelled, &by_spelling) != 0 ||
			    stat(path, &by_path) != 0 ||
			    by_spelling.st_ino != by_path.st_ino) {
				printf("skipping %s, this platform does not"
				       " answer it\n", spelled);
				check_accepted("held.bin", spelled);
				continue;
			}
			snprintf(expected, sizeof(expected),
			         "error: -q and -d cannot both write to one file"
			         " (held.bin and %s)\n", spelled);
			check_refused("held.bin", spelled, expected);
		}
	}
	/* A zero-padded number is the platform's to answer, not this code's:
	 * the descriptor filesystem the BSDs mount reads it as the same
	 * descriptor, and on Linux /dev/fd is a directory of exact names where
	 * the path leads nowhere. Whichever this platform does, the pair must
	 * follow it -- reading the number alone would make a destination that
	 * cannot be created a silent redirect to standard output. */
	{
		char padded[64];
		struct stat by_padded, by_path;

		snprintf(padded, sizeof(padded), "/dev/fd/0%d", held);
		if (stat(padded, &by_padded) == 0 && stat(path, &by_path) == 0 &&
		    by_padded.st_ino == by_path.st_ino) {
			char expected[PATH_MAX + 128];

			snprintf(expected, sizeof(expected),
			         "error: -q and -d cannot both write to one file"
			         " (held.bin and %s)\n", padded);
			check_refused("held.bin", padded, expected);
		} else {
			check_accepted("held.bin", padded);
		}
	}
	/* The fixed names of the other two descriptors are resolved the same
	 * way. Nothing writes a capture to standard error on purpose, but a
	 * destination has to be identified whatever stream it is on, and
	 * "/dev/stderr" beside "/dev/fd/2" is one file. Standard error is a
	 * file of this test's own while the refusal is being captured, which is
	 * what the two resolve to. */
	{
		struct stat by_name, by_number;

		if (stat("/dev/stderr", &by_name) == 0 &&
		    fstat(STDERR_FILENO, &by_number) == 0 &&
		    by_name.st_ino == by_number.st_ino)
			check_refused("/dev/stderr", "/dev/fd/2",
			              "error: -q and -d cannot both write to one"
			              " file (/dev/stderr and /dev/fd/2)\n");
		else
			printf("skipping /dev/stderr, this platform does not"
			       " answer it\n");
		/* And standard input, which is this test's own descriptor 0.
		 * Nothing writes a capture there either; it is identified for
		 * the same reason. */
		if (stat("/dev/stdin", &by_name) == 0 &&
		    fstat(STDIN_FILENO, &by_number) == 0 &&
		    by_name.st_ino == by_number.st_ino)
			check_refused("/dev/stdin", "/dev/fd/0",
			              "error: -q and -d cannot both write to one"
			              " file (/dev/stdin and /dev/fd/0)\n");
		else
			printf("skipping /dev/stdin, this platform does not"
			       " answer it\n");
	}
	/* Another file on the same descriptor path is still another file. */
	make_file("other-held.bin", "elsewhere\n");
	check_accepted("other-held.bin", path);
	close(held);
	/* With the descriptor closed the path names nothing again. */
	check_accepted("held.bin", path);
}

/* Everything this test made, in the working directory it made for itself. */
static void clean_up(void)
{
	static const char *const made[] = {
		"real.bin", "link.bin", "hard.bin", "other.bin", "held.bin",
		"dangling.bin", "sink.bin", "redirected.bin", "-",
		"to-held", "other-held.bin",
		"points-at-dash", "points-at-stream", "elsewhere.bin",
		"chain-1", "chain-2", "chain-ghost.bin", "loop",
		"chain-end.bin", "deep.bin", "into-sub",
		"sub/pointer.bin", "sub/updown.bin", "sub/out",
	};
	char sub[sizeof(work) + 8];
	size_t i;

	char link[64];
	int n;

	for (i = 0; i < sizeof(made) / sizeof(made[0]); i++)
		unlink(made[i]);
	/* The two chains built for the hop bound. */
	for (n = 1; n <= 17; n++) {
		snprintf(link, sizeof(link), "hop16-%d", n);
		unlink(link);
		snprintf(link, sizeof(link), "hop17-%d", n);
		unlink(link);
	}
	if (chdir("/") != 0)
		return;
	snprintf(sub, sizeof(sub), "%s/sub", work);
	rmdir(sub);
	rmdir(work);
}

int main(void)
{
	if (mkdtemp(work) == NULL || chdir(work) != 0) {
		fprintf(stderr, "FAIL cannot work in %s: %s\n", work,
		        strerror(errno));
		return 1;
	}
	if (mkdir("sub", 0700) != 0) {
		fprintf(stderr, "FAIL cannot create sub: %s\n", strerror(errno));
		return 1;
	}

	test_one_name_written_twice();
	test_two_names_are_two_files();
	test_one_entry_of_one_directory();
	test_nothing_to_compare();
	test_links_to_one_file();
	test_a_link_to_a_file_not_there_yet();
	test_what_a_writer_would_not_overwrite();
	test_an_empty_value();
	test_a_chain_of_links();
	test_standard_output_is_left_alone();
	test_the_stream_beside_the_file_called_dash();
	test_standard_output_pointed_at_a_file();
	test_descriptor_paths();

	/* Left behind only when a check failed, so the files can be looked at;
	 * a clean run takes its directory away. */
	if (failures == 0)
		clean_up();

	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
