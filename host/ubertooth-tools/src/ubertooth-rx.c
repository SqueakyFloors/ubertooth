/*
 * Copyright 2010, 2011 Michael Ossmann
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "opt_parse.h"
#include "request_report.h"
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How many boards one capture may read at once. Each one costs a 72 MB ring
 * buffer and a USB transfer of its own, and the band holds 79 channels, so
 * this is only here to stop a mistyped list from asking for hundreds. */
#define RX_MAX_BOARDS 8

static void usage(FILE *file)
{
	fprintf(file, "ubertooth-rx - passive Classic Bluetooth discovery/decode\n");
	fprintf(file, "\n");
	fprintf(file, "Example usage:\n");
	fprintf(file, "\tubertooth-rx -- sniff for all LAPs\n");
	fprintf(file, "\tubertooth-rx -l <lap> -- calculate UAP for a given LAP\n");
	fprintf(file, "\tubertooth-rx -l <lap> -u <uap> -- calculate clock and follow piconet\n");
	fprintf(file, "\tubertooth-rx -z -t 20 -- survey mode: discover all LAPs+UAPs for 20 seconds\n");
	fprintf(file, "\n");
	fprintf(file, "Major modes:\n");
	fprintf(file, "\t-l <LAP> to decode (6 hex) - if not specified sniff all LAPs\n");
	fprintf(file, "\t-u <UAP> to decode (2 hex) - if not specified calculate UAP (requires LAP)\n");
	fprintf(file, "\t-z Survey mode - discover and list piconets (interrupt with ctrl-C)\n");
	fprintf(file, "\t-i <filename> replay a capture saved with -d, with its recorded times\n");
	fprintf(file, "\t              - if not specified use Ubertooth for live capture\n");
	fprintf(file, "\n");
	fprintf(file, "Known limitation: the clock search does not confirm a clock against a\n");
	fprintf(file, "piconet that uses adapted hopping (AFH), which every controller tested on\n");
	fprintf(file, "this project's bench does, so following such a piconet does not start. Its\n");
	fprintf(file, "addresses, and the packets of it the boards hear, are still recovered. See\n");
	fprintf(file, "ubertooth-rx(1).\n");
	fprintf(file, "\n");
	fprintf(file, "Configuration:\n");
	fprintf(file, "\t-c <0-%d> set a fixed bluetooth channel [Default: sweep]\n",
	        NUM_BREDR_CHANNELS - 1);
	fprintf(file, "\t   - one per board, comma separated, to spread them out\n");
	fprintf(file, "\t-s sweep all channels - the default; a -c channel wins over it\n");
	fprintf(file, "\t-e max_ac_errors (default: %d, range: 0-%d)\n",
	        max_ac_errors, MAX_AC_ERRORS);
	fprintf(file, "\t-t <SECONDS> sniff timeout - 0 means no timeout [Default: 0]\n");
	fprintf(file, "\n");
	fprintf(file, "Output options:\n");
	fprintf(file, "\t-r <filename> capture packets to PcapNG file\n");
	fprintf(file, "\t-q <filename> capture packets to PCAP file\n");
	fprintf(file, "\t-d <filename> dump packets to binary file\n");
	fprintf(file, "\n");
	fprintf(file, "Miscellaneous:\n");
	fprintf(file, "\t-V print version information\n");
	fputs(OPT_USAGE_SELECT_DEVICE, file);
	fprintf(file, "\t   - up to %d boards, comma separated, read together\n",
	        RX_MAX_BOARDS);
}

/* One board of a capture: the handle, the piconet it feeds, and what it was
 * asked to tune to.
 *
 * A piconet each, not one between them. libbtbb anchors a piconet's clock
 * recovery on the first packet's CLKN and works in differences from it, and
 * two boards' CLKN counters are unrelated, so a packet from the second board
 * would be placed at a point in the hop pattern it was never heard at. What
 * the boards share is what they have worked out -- the channels the piconet
 * has been heard on, and its upper address part -- which needs no common
 * clock: see rx_share_findings(). The piconet itself is the shared time base,
 * and each board recovers its clock from its own packets.
 *
 * pn is NULL for a survey, which keeps a piconet per address inside libbtbb,
 * and for a run given no -l. */
struct rx_board {
	ubertooth_t* ut;
	btbb_piconet* pn;
	/* The channel this board was asked for, in MHz, or the value above the
	 * band that asks for the sweep. */
	uint16_t channel;
	/* Whether this board's upper address part (UAP) came from another
	 * board rather than from its own packets. Such a board is not evidence
	 * for that value: see rx_share_findings(). */
	int uap_handed;
	/* The UAP this board worked out from its own packets, recorded the
	 * first time it held one it was not handed. What a board holds at the
	 * end of a run is not necessarily what it worked out, so this is the
	 * evidence the agreement and the end-of-run report rest on; see
	 * rx_share_findings(). */
	int uap_found;
	uint8_t uap_found_value;
};

/* The AFH channel map is ten bytes, bit b of byte i being channel 8i + b: the
 * order the Core specification's Set_AFH_Host_Channel_Classification and
 * libbtbb's own printed map both use. libubertooth says the same length for
 * the board's map, and the merge below reads libbtbb's through that constant,
 * so the two have to agree. */
#if AFH_MAP_BYTES != 10
#error "AFH_MAP_BYTES is not 10; libbtbb's AFH map is ten bytes"
#endif

/* The capture files this run was asked for, by the names the user gave. */
struct rx_files {
	const char* pcapng;  /* -r */
	const char* pcap;    /* -q */
	const char* dump;    /* -d */
};

/* Takes the shared capture writers off every board but the first.
 *
 * The writers live on the first board's handle, and every other board's handle
 * points at the same ones so that a packet from any board reaches the one
 * file. A teardown writes out and closes whatever its handle holds, so without
 * this one file would be closed once per board. */
static void rx_unshare_files(struct rx_board* boards, int n_boards)
{
	int i;

	for (i = 1; i < n_boards; i++) {
		if (boards[i].ut == NULL)
			continue;
		boards[i].ut->h_pcap_bredr = NULL;
		boards[i].ut->h_pcapng_bredr = NULL;
	}
}

/* Gives every board this run opened back and forgets it.
 *
 * `stop` sends the teardown's STOP, for a board that may be left in a mode a
 * request has started; without it the board is only closed, which is right
 * where no mode has started yet and where a board that has stopped answering
 * would turn the STOP into a second failure in the report. */
static void rx_give_back_boards(struct rx_board* boards, int n_boards, int stop)
{
	int i;

	rx_unshare_files(boards, n_boards);
	for (i = n_boards - 1; i >= 0; i--) {
		ubertooth_t* ut = boards[i].ut;

		/* Before the handle, and on every path, so a run that ends
		 * where a board refused a request gives back as much as one
		 * that captured. */
		if (boards[i].pn != NULL) {
			btbb_piconet_unref(boards[i].pn);
			boards[i].pn = NULL;
		}
		if (ut == NULL)
			continue;
		if (stop)
			ubertooth_stop(ut);
		else
			ubertooth_close(ut);
		ubertooth_free(ut);
		boards[i].ut = NULL;
	}
}

/* Whether the capture is ending: the user's Ctrl-C, the -t timer, or a board
 * whose receive stream failed. ubertooth_stopping() is what folds the first two
 * into a handle, so every board is asked rather than stopping at the first that
 * says yes.
 *
 * Any one board ends the run. A capture that has lost one of its boards is no
 * longer the capture that was asked for: the channels it was covering go
 * unheard and the merge below has one fewer source, so it ends and the exit
 * status says so. */
static int rx_stopping(struct rx_board* boards, int n_boards)
{
	int i, stopping = 0;

	for (i = 0; i < n_boards; i++)
		if (ubertooth_stopping(boards[i].ut))
			stopping = 1;
	return stopping;
}

/* The words of a comma-separated list, in order. They point into `text`, which
 * this writes a NUL over each comma of, so the caller's buffer has to outlive
 * them. Returns how many words there were, or -1 with an error line for an
 * empty word or a list longer than `max`: "-U 0,,1" and a mistyped list of
 * fifty are both named rather than half accepted. */
static int rx_split_list(char* text, const char* what, char** words, int max)
{
	int n = 0;
	char* p = text;

	for (;;) {
		char* comma = strchr(p, ',');

		if (comma != NULL)
			*comma = '\0';
		if (*p == '\0') {
			fprintf(stderr, "error: the %s list has an empty "
			        "entry\n", what);
			return -1;
		}
		if (n == max) {
			fprintf(stderr, "error: at most %d %s%s can be given\n",
			        max, what, max == 1 ? "" : "s");
			return -1;
		}
		words[n++] = p;
		if (comma == NULL)
			return n;
		p = comma + 1;
	}
}

/* Notes the UAP a board worked out for itself, once. Called before anything is
 * shared, so a board that has just settled one is evidence on the same pass. */
static void rx_note_own_uap(struct rx_board* board)
{
	if (board->uap_found || board->uap_handed)
		return;
	if (!btbb_piconet_get_flag(board->pn, BTBB_UAP_VALID))
		return;
	board->uap_found = 1;
	board->uap_found_value = btbb_piconet_get_uap(board->pn);
}

/* Hands every board what the others have found, which is the whole of what
 * boards reading one piconet share.
 *
 * The channel maps join. btbb_piconet_set_channel_seen() records only that a
 * channel carried this piconet's traffic, so one board's map ORs with
 * another's whatever clock either board is on, and btbb_piconet_set_afh_map()
 * rebuilds the hop tables from the union. Each board then narrows its clock
 * candidates against a map that filled as many times faster as there are
 * boards, and an incomplete map is what makes a wrong clock look right: the
 * candidates are narrowed against hops the piconet never made.
 *
 * The upper address part (UAP) is a scalar, and it is passed on once two
 * boards have worked out the same one from their own packets. Not the first
 * board's: the library settles a UAP on as few as two packets and is sometimes
 * wrong about it -- a bench run settled on 0xdb where the peer's is 0x0d --
 * and a wrong one handed over is not something the board it was handed to can
 * get out of. The library restores the UAP it was filtering on whenever the
 * clock search runs out of candidates, so that board filters on the wrong
 * value for the rest of the run and works out nothing of its own. Two boards
 * settling on the same value from different packets is a cross-check a single
 * board cannot make, and it is what makes passing the value on worth doing.
 * A board whose UAP was handed to it is not evidence for it. The value goes
 * only to a board that has worked out none of its own, and only once; a board
 * is never taken off its own answer, for the reason the loop below gives.
 *
 * The clock is not shared and does not need to be: it is the piconet's own, so
 * each board recovers it from its own packets once it has the address. Two
 * boards agreeing on one is evidence a single board cannot produce.
 *
 * `merged` carries the union between calls, so a map is pushed only when it has
 * grown. One board shares nothing: its piconet already holds everything it
 * knows, and pushing the map back would only repeat the work
 * btbb_piconet_set_channel_seen() has just done. */
static void rx_share_findings(struct rx_board* boards, int n_boards,
                              uint8_t* merged)
{
	uint8_t next[AFH_MAP_BYTES];
	int grown = 0, agreed = 0, i, j;
	uint8_t uap = 0;

	if (n_boards < 2)
		return;

	memcpy(next, merged, sizeof(next));
	for (i = 0; i < n_boards; i++) {
		const uint8_t* map = btbb_piconet_get_afh_map(boards[i].pn);

		for (j = 0; j < (int)sizeof(next); j++)
			next[j] |= map[j];
	}
	if (memcmp(next, merged, sizeof(next)) != 0) {
		memcpy(merged, next, sizeof(next));
		grown = 1;
	}

	/* Two boards that worked out the same UAP from their own packets. At
	 * most eight boards, so comparing every pair is a few dozen reads. */
	for (i = 0; i < n_boards; i++)
		rx_note_own_uap(&boards[i]);
	for (i = 0; i < n_boards && !agreed; i++) {
		if (!boards[i].uap_found)
			continue;
		for (j = i + 1; j < n_boards; j++) {
			if (boards[j].uap_found &&
			    boards[j].uap_found_value ==
			    boards[i].uap_found_value) {
				uap = boards[i].uap_found_value;
				agreed = 1;
				break;
			}
		}
	}

	for (i = 0; i < n_boards; i++) {
		if (grown)
			btbb_piconet_set_afh_map(boards[i].pn, merged);
		/* Only to a board that has worked out none of its own, and
		 * only once. A board is never taken off its own answer, even
		 * when two others disagree with it: btbb_piconet_set_uap()
		 * writes the address part and nothing else, so a board whose
		 * clock search has already chosen its candidates would go on
		 * narrowing a list picked for the old address and could reach
		 * a clock belonging to it, which this tool would then put the
		 * board on. The run reports the disagreement instead and
		 * leaves that board to the library. Handing a known UAP to
		 * every board is what -u is for.
		 *
		 * Once, because a board whose UAP the library has cleared to
		 * start discovery over would otherwise be handed the same
		 * value back before a single packet arrived. */
		if (!agreed || boards[i].uap_found || boards[i].uap_handed)
			continue;
		if (btbb_piconet_get_flag(boards[i].pn, BTBB_UAP_VALID))
			continue;
		btbb_piconet_set_uap(boards[i].pn, uap);
		boards[i].uap_handed = 1;
	}
}

/* What the boards found between them, once the capture has ended: the address
 * part, and the channels this piconet was heard on across all of them. A
 * single board's result is its own piconet's and libbtbb reports it already,
 * so this is said only where there was something to merge.
 *
 * On standard error, as the library's own discovery notes are, so a capture
 * writing to standard output is unaffected. The map reads in the order
 * btbb_print_afh_map() and hcitool print it: byte 0 first, bit b of byte i
 * being channel 8i + b.
 *
 * told_uap says the user gave the address part with -u, in which case there is
 * nothing to report about it: every board was started on that value. */
static void rx_report_findings(int n_boards, const uint8_t* merged,
                               const struct rx_board* boards, int told_uap)
{
	int i, used = 0, have_uap = 0, disagree = 0;
	uint8_t uap = 0, other = 0;

	for (i = 0; i < NUM_BREDR_CHANNELS; i++)
		if (merged[i / 8] & (1 << (i % 8)))
			used++;

	/* Only what a board worked out for itself, read from what was recorded
	 * when it did: a board holding a value another board handed it would
	 * otherwise read as a second board agreeing, which is the cross-check
	 * this line is for, and what a board holds at the end is not
	 * necessarily what it found. */
	for (i = 0; i < n_boards; i++) {
		if (!boards[i].uap_found)
			continue;
		if (!have_uap) {
			uap = boards[i].uap_found_value;
			have_uap = 1;
		} else if (boards[i].uap_found_value != uap) {
			disagree = 1;
			other = boards[i].uap_found_value;
		}
	}

	/* Nothing to report where the user gave the UAP with -u: every board
	 * was started on it, so the line would hand their own value back as a
	 * finding. */
	if (told_uap)
		;
	else if (!have_uap)
		fprintf(stderr, "note: %d boards: no UAP found\n", n_boards);
	else if (disagree)
		/* One of them is wrong, and this run cannot say which: the
		 * library settles a UAP on the packets it has had, and two
		 * boards have had different packets. Both values are named,
		 * because the run acted on neither: a board is never taken
		 * off its own answer. */
		fprintf(stderr, "note: %d boards: the boards worked out "
		        "different UAPs, 0x%02x and 0x%02x among them; one of "
		        "them is wrong, and each board kept its own\n",
		        n_boards, uap, other);
	else
		fprintf(stderr, "note: %d boards: UAP = 0x%02x\n", n_boards,
		        uap);

	fprintf(stderr, "note: %d boards: AFH map: 0x", n_boards);
	for (i = 0; i < AFH_MAP_BYTES; i++)
		fprintf(stderr, "%02x", merged[i]);
	fprintf(stderr, ", %d of %d channels heard\n", used,
	        NUM_BREDR_CHANNELS);
}

/* Names the request a board refused, says why and what to try, and gives
 * every board back rather than capturing with settings the user did not ask
 * for. A refused request is not proof that the board never got it -- a
 * transfer can time out after it arrived -- so the message says only what the
 * tool knows. */
static int request_failed(struct rx_board* boards, int n_boards,
                          const char* what, int code)
{
	report_refused_request(what, code);
	rx_give_back_boards(boards, n_boards, 0);
	return 1;
}

/* Takes the capture files away again, for a failure between creating them and
 * the first packet: a refused run then leaves no empty capture behind.
 *
 * The handles say which files exist, since each is set only once its writer
 * has the file open, so before the files are created this does nothing. It
 * runs before the board goes back, because that is what closes those writers:
 * the files go by name while the writers still hold them, and the header
 * bytes a writer had buffered then go nowhere. Which names are left alone is
 * opt_discard_capture_file()'s decision. */
static void rx_discard_files(ubertooth_t* ut, const struct rx_files* files)
{
	if (ut != NULL && ut->h_pcapng_bredr != NULL)
		opt_discard_capture_file(files->pcapng);
	if (ut != NULL && ut->h_pcap_bredr != NULL)
		opt_discard_capture_file(files->pcap);
	if (dumpfile != NULL)
		opt_discard_capture_file(files->dump);
}

/* Gives the board back for a failure after the capture files were created but
 * before any mode has started, taking those files with it. The caller has
 * already said what failed. Closed without a STOP for the same reason
 * request_failed() does not send one. */
static int setup_failed(struct rx_board* boards, int n_boards,
                        const struct rx_files* files)
{
	rx_discard_files(boards[0].ut, files);
	rx_give_back_boards(boards, n_boards, 0);
	return 1;
}

/* The same for a failure while the receive transfer is being set up, which the
 * board is given back from with a STOP: by then it may be left in the mode an
 * earlier command started. libubertooth or the caller has already said what
 * failed. Every failure from the files down to the capture loop goes through
 * one of these three helpers, so a path added between them cannot forget the
 * files. */
static int transfer_failed(struct rx_board* boards, int n_boards,
                           const struct rx_files* files)
{
	rx_discard_files(boards[0].ut, files);
	rx_give_back_boards(boards, n_boards, 1);
	return 1;
}

/* The same report for a request refused once the capture is already running.
 * The mode is stopped and the board given back, so a board the user has to
 * rescue is not left streaming with settings they did not ask for.
 *
 * Unlike request_failed() above, which says nothing to a board that has not
 * answered, this does send the STOP, and a board that has stopped answering
 * can make it print a line of its own. A mode left running is worse than a
 * second line in the report. Both man pages say so.
 *
 * The poll thread is running by now and nothing else joins it:
 * ubertooth_stop() frees the libusb context it polls. */
static int capture_failed(struct rx_board* boards, int n_boards,
                          const struct rx_files* files,
                          const char* what, int code)
{
	report_refused_request(what, code);
	rx_discard_files(boards[0].ut, files);
	ubertooth_bulk_thread_stop();
	rx_give_back_boards(boards, n_boards, 1);
	return 1;
}

/* Set once a replay has read a whole record from its input. That is the line
 * its capture files are kept from, the same line a capture draws at its first
 * packet: the replay ran, and what the decoder made of the record -- a packet,
 * or nothing it recognized -- is the result. Before the first record the files
 * hold their headers and nothing else. */
static int replay_read_a_record;

/* How long the clock search may run without confirming a clock before the tool
 * says what it does not reach, in milliseconds.
 *
 * The search cannot report this itself, because it does not fail: it rules its
 * candidates out and starts over, printing a candidate count each time, so
 * nothing a user sees separates a search that is getting somewhere from one
 * that is not going to finish. The tool says so once instead.
 *
 * Half a minute. A search that does reach a clock reaches it within a few
 * seconds of settling CLK1-6, because every packet of the piconet rules out
 * most of what is left, so half a minute is several times over what a working
 * search needs and well short of the minute a working follow is given above.
 * Saying it too early is the worse mistake of the two, which is what sets the
 * span: a user told that a search is not going to finish will stop a run that
 * was about to succeed.
 *
 * Nothing acts on it. The search carries on exactly as before and the note
 * only says what the run is going to reach. */
#define CLOCK_SEARCH_NOTE_MS 30000

/* Says once what the clock search does not reach, for a run whose search has
 * been going that long with no clock confirmed. Called where the capture
 * decodes, so a run with no board, and one whose piconet is silent, say
 * nothing: the reading is -1 until a packet of the piconet reaches the search.
 *
 * On standard error, with the rest of the notes, so a capture that has taken
 * standard output still carries only the capture. */
static int clock_search_noted;

static void rx_note_clock_search_limit(ubertooth_t* ut)
{
	long ms = ubertooth_clock_search_ms(ut);

	if (clock_search_noted || ms < CLOCK_SEARCH_NOTE_MS)
		return;
	clock_search_noted = 1;
	/* How long the search has really run, not the span that brought it
	 * here: a run that reaches this late -- a replay reading its records
	 * at the times they were recorded, say -- would otherwise report a
	 * search of minutes as half a minute. */
	fprintf(stderr, "note: the clock search has run for %ld s without "
	        "confirming a clock; it does not confirm one against a piconet "
	        "that uses adapted hopping (AFH)\n", ms / 1000);
	fputs("note: the piconet's LAP and UAP, and the packets of it the "
	      "boards hear, are still recovered, decoded, and captured; see "
	      "ubertooth-rx(1)\n", stderr);
}

/* A replay's decode: notes the record, then decodes it like any other. The
 * decode is libubertooth's cb_rx() and notes nothing itself, and a live
 * capture needs no note, so only a replay goes through here. */
static void replay_decode(ubertooth_t* ut, void* args)
{
	replay_read_a_record = 1;
	cb_rx(ut, args);
	/* A replay reads its records at the times they were recorded, so a
	 * search over a recorded capture runs for as long as it did live and
	 * the note lands in the same place. */
	rx_note_clock_search_limit(ut);
}

/* How long a follow may hear nothing of its piconet before the clock it is
 * following is given up, in milliseconds.
 *
 * Silence is the only symptom a wrong clock has, rather than anything that
 * fails (see ubertooth_follow_quiet_ms()), so the value is a judgement about
 * what a silence means, and it is made here because only this tool knows it
 * asked the board to hop with the piconet.
 *
 * A minute is 48000 slot pairs, and the right clock is on the piconet's
 * channel for every one of them, so a link carrying even one packet a second
 * would have been heard from sixty times before the wait is up. Nothing a
 * working link does leaves a gap that long either: the longest interval the
 * low-power sniff mode allows is about 41 s.
 *
 * It has to be that long, because giving a clock up rules it out: a follow
 * that was right and merely went quiet does not get that clock back in the
 * same run, where a wrong clock kept longer costs only time. The price of the
 * length is that a wrong clock is given up only on a link slow enough for one
 * packet in eighteen to leave a minute's gap, which is about one packet a
 * second or less. */
#define FOLLOW_QUIET_MS 60000

int main(int argc, char* argv[])
{
	int opt, have_lap = 0, have_uap = 0;
	int survey_mode = 0;
	/* Whether -c named a channel, which decides only how a refused channel
	 * request is named: a command line without it asks for the sweep. */
	int have_channel = 0;
	/* Whether -t named a timeout. A timeout of 0 means "no timeout", so the
	 * value cannot say whether the option was given. */
	int have_timeout = 0;
	int exit_status = 0;
	const char* infile_name = NULL;
	/* The three names together, for the failures that have to take back a
	 * file this run created. Filled in once the command line is read. */
	struct rx_files files = { NULL, NULL, NULL };
	/* Files named on the command line, opened after every check passes. */
	const char* pcapng_name = NULL;
	const char* pcap_name = NULL;
	const char* dump_name = NULL;
	/* -h and -V both end the command once the whole command line has
	 * parsed, and a command line with both prints both (see below).
	 * version_status is -V's own result: it reads the board, so it can fail
	 * where -h cannot. */
	int do_help = 0, do_version = 0, version_status = 0;
	opt_once once = {0};
	int r, i;
	int timeout = 0;
	/* The -U and -c lists as the user wrote them, split in place once the
	 * whole command line has parsed. */
	char* ubertooth_devices = NULL;
	char* channel_arg = NULL;
	/* The boards this capture reads, in the order -U named them, and the
	 * selectors for them. One board unless -U named more. */
	struct rx_board boards[RX_MAX_BOARDS];
	char* selectors[RX_MAX_BOARDS];
	int n_boards = 1;
	/* The first board's piconet, for the parts of the run that are about
	 * the one piconet rather than about a board: the address recorded in
	 * the capture file, and the survey results. */
	btbb_piconet* pn = NULL;
	/* The channels the boards were heard on, across all of them. Reported
	 * when the capture ends. */
	uint8_t merged_map[AFH_MAP_BYTES];
	uint32_t lap = 0;
	uint8_t uap = 0;
	/* Above the Bluetooth band, which the firmware reads as "sweep the
	 * whole band": the default when no -c names a fixed channel. */
	uint16_t channel = 9999;

	/* The first board's handle, for the steps that are about this run
	 * rather than about one board: the capture files, the timer, and the
	 * replay. Allocated once the whole command line has been accepted
	 * (below): every refusal above returns without a board, and each handle
	 * carries a 72 MB fifo. */
	ubertooth_t* ut = NULL;

	/* Where the survey results go: standard output, unless a capture on "-"
	 * has taken it, in which case they would go to /dev/null with the rest
	 * of this process's standard output. The survey is what -z was asked
	 * for, so it is printed on standard error rather than lost. */
	FILE* report_stream = stdout;

	/* The tool reports a bad option itself, in the usual one-line form:
	 * see opt_report_bad_option(). The leading ':' makes a missing option
	 * value distinguishable from an option getopt does not know. */
	/* Named, so the loop below and opt_report_bad_option()
	 * cannot be given different letters. */
	const char options[] = ":hVi:l:u:U:d:e:r:sq:t:zc:";

	memset(boards, 0, sizeof(boards));
	memset(merged_map, 0, sizeof(merged_map));
	/* One board, opened by whichever name -U gave, or whichever the library
	 * finds when it gave none. */
	memset(selectors, 0, sizeof(selectors));

	opterr = 0;
	while ((opt=getopt(argc, argv, options)) != EOF) {
		/* No option may be given a value twice: see opt_check_once(). */
		if (opt_check_once(&once, opt, optarg) < 0)
			return 1;
		switch(opt) {
		case 'i':
			infile_name = optarg;
			break;
		case 'l':
			if (opt_require_lap(optarg, &lap) < 0)
				return 1;
			have_lap++;
			break;
		case 'u':
			if (opt_require_uap(optarg, &uap) < 0)
				return 1;
			have_uap++;
			break;
		case 'U':
			ubertooth_devices = optarg;
			break;
		/* The capture and dump files are named here and opened once the
		 * whole command line has been accepted, so a refused command
		 * leaves the filesystem alone. */
		case 'r':
			/* A PcapNG writer seeks back to finish the blocks it has
			 * written, which standard output cannot do, so a lone "-"
			 * is refused here rather than taken as the file name "-".
			 * The PCAP writer does not seek: -q takes it. */
			if (strcmp(optarg, "-") == 0) {
				fputs("error: PCAPNG capture to stdout is not supported;"
				      " use -q -\n", stderr);
				return 1;
			}
			pcapng_name = optarg;
			break;
		case 'q':
			pcap_name = optarg;
			break;
		case 'd':
			dump_name = optarg;
			break;
		case 'e':
			if (opt_require_int(optarg, 0, MAX_AC_ERRORS,
			                    "maximum access code errors", "",
			                    &max_ac_errors) < 0)
				return 1;
			break;
		case 's':
			/* The sweep is what the tool does when no -c names a
			 * fixed channel, so -s has nothing to select: it is
			 * accepted so a command line can state the default. */
			break;
		case 't':
			if (opt_require_count(optarg, 0, "timeout", "seconds",
			                      "0 for none", &timeout) < 0)
				return 1;
			have_timeout = 1;
			break;
		case 'z':
			++survey_mode;
			break;
		case 'c':
			/* Read once the whole command line has parsed: it may
			 * name one channel per board, so how many are allowed
			 * depends on -U, which may come after it. */
			channel_arg = optarg;
			have_channel = 1;
			break;
		case 'V':
			do_version = 1;
			break;
		case 'h':
			do_help = 1;
			break;
		default:
			/* A command line the tool will not take: the one-line
			 * reason goes to standard error and standard output
			 * stays empty. The tool's other output lines are
			 * converted separately. */
			opt_report_bad_option(opt, argc, argv, options);
			return 1;
		}
	}

	/* ubertooth-rx takes no operands: refuse a leftover word before any
	 * device is opened, rather than running the rest of the command. */
	if (opt_check_no_operands(argc, argv) < 0)
		return 1;

	/* The boards, and then the channels for them. A capture may read
	 * several boards at once: each hears one 1 MHz channel at a time, so
	 * one parked on a channel sees about one packet in 79 of a connection,
	 * and every step of following one waits on those sightings. */
	if (ubertooth_devices != NULL) {
		n_boards = rx_split_list(ubertooth_devices, "board",
		                         selectors, RX_MAX_BOARDS);
		if (n_boards < 0)
			return 1;
		/* The same board twice would be opened twice, and the second
		 * open would fail with whatever libusb says about a board
		 * already claimed, which tells the user nothing about their
		 * command line. Naming it once and reading it once is what
		 * they meant. A board named by index and by serial is the same
		 * board and is not caught here; the open reports that. */
		for (i = 1; i < n_boards; i++) {
			int j;

			for (j = 0; j < i; j++) {
				if (strcmp(selectors[i], selectors[j]) != 0)
					continue;
				fprintf(stderr, "error: board %s is named "
				        "twice\n", selectors[i]);
				return 1;
			}
		}
	}

	/* One channel for every board, or one for all of them. Anything else is
	 * a command line whose boards and channels do not line up, and guessing
	 * which board got which channel is not something a user could read back
	 * off the output. */
	if (channel_arg != NULL) {
		char* words[RX_MAX_BOARDS];
		/* No more channels than there are boards, so a list longer
		 * than that is refused as too long rather than reported
		 * against the limit of eight. */
		int n_channels = rx_split_list(channel_arg, "channel", words,
		                               n_boards);

		if (n_channels < 0)
			return 1;
		if (n_channels != 1 && n_channels != n_boards) {
			fprintf(stderr, "error: -c names %d channel%s for %d "
			        "board%s; give one each or one for all\n",
			        n_channels, n_channels == 1 ? "" : "s",
			        n_boards, n_boards == 1 ? "" : "s");
			return 1;
		}
		for (i = 0; i < n_boards; i++) {
			int bt_channel;

			if (opt_require_int(words[n_channels == 1 ? 0 : i], 0,
			                    NUM_BREDR_CHANNELS - 1,
			                    "channel", "", &bt_channel) < 0)
				return 1;
			boards[i].channel = (uint16_t)(bt_channel + BT_FREQ_MIN);
		}
	} else {
		for (i = 0; i < n_boards; i++)
			boards[i].channel = channel;
	}

	/* -h and -V end the command, but only once the whole command line
	 * has parsed: a bad option value or a stray word is reported even
	 * alongside them. A command line with both prints both, and the usage
	 * text comes last so the version is not scrolled off the screen by
	 * it; the order they were written in does not matter. */
	/* -V reports one board, as its own line of the SYNOPSIS says: the
	 * answer carries the host tools' version once and then the firmware of
	 * the board named, and with no -U at all it already reports every
	 * board attached. A list is for a capture, so one beside -V is a
	 * command line that cannot be answered as asked. */
	if (do_version && n_boards > 1) {
		fprintf(stderr, "error: -V reports one board; name one with -U,"
		        " or none to report every board attached\n");
		return 1;
	}

	if (do_version)
		version_status = ubertooth_print_versions(stdout, selectors[0]);
	if (do_help)
		usage(stdout);
	if (do_version || do_help)
		return version_status;

	if(survey_mode && (have_lap || have_uap)) {
		fprintf(stderr, "error: survey mode (-z) discovers addresses, so it"
		        " cannot be given -l or -u\n");
		return 1;
	}

	/* Standard output carries one stream, so only one capture can have it:
	 * a PCAP file and a packet dump interleaved there would be neither. */
	if (pcap_name != NULL && dump_name != NULL &&
	    strcmp(pcap_name, "-") == 0 && strcmp(dump_name, "-") == 0) {
		fputs("error: -q and -d cannot both write to standard output\n",
		      stderr);
		return 1;
	}

	/* A UAP is only usable with the LAP it belongs to. This is an argument
	 * error like the survey check above, so it is refused here rather than
	 * after a device has been opened, where it used to depend on whether a
	 * board happened to be free. (Survey mode has already returned for any
	 * address, so it cannot reach this.) */
	if (have_uap && !have_lap) {
		fprintf(stderr, "error: -u <UAP> requires -l <LAP>\n");
		return 1;
	}

	/* A replay has no board to tune and nothing to wait for, so these two
	 * ask for something it cannot do. They used to be accepted and dropped
	 * without a word: the file was read on no channel and with no timeout,
	 * and the user had no way to tell. */
	if (infile_name != NULL && have_channel) {
		fprintf(stderr, "error: -c cannot be combined with -i; a replay"
		        " reads a saved capture rather than tuning a board\n");
		return 1;
	}
	if (infile_name != NULL && n_boards > 1) {
		fprintf(stderr, "error: -i names one saved capture, so it"
		        " cannot be given several boards to read\n");
		return 1;
	}

	/* A dump record carries no board, and -i hands a whole dump to one
	 * piconet, so a dump holding several boards' records would be replayed
	 * as one board's: the clock each record was stamped with is its own
	 * board's, and placing another board's record in that recovery puts it
	 * at a point in the hop pattern it was never heard at. There is no
	 * right way to replay such a file, so it is not written. The PCAP and
	 * PCAPNG captures are for a reader rather than for this tool's
	 * recovery and do take every board; see SEVERAL BOARDS AT ONCE. */
	if (dump_name != NULL && n_boards > 1) {
		fprintf(stderr, "error: -d writes one dump for -i to replay as"
		        " one board, so it cannot be given several boards\n");
		return 1;
	}
	if (infile_name != NULL && have_timeout) {
		fprintf(stderr, "error: -t cannot be combined with -i; a replay"
		        " reads a saved capture rather than waiting for a"
		        " board\n");
		return 1;
	}

	/* A capture bound for standard output must not go to a terminal. That
	 * is a problem with the command line, so it is refused here, before a
	 * board is opened; the stream itself is taken over further down, where
	 * the file is created. */
	if (opt_check_capture_destination(pcap_name, "binary capture data") < 0 ||
	    opt_check_capture_destination(dump_name, "binary dump data") < 0)
		return 1;

	/* Every check has passed, so the handles and their 72 MB fifos can be
	 * allocated, one per board: the capture files below are kept on the
	 * first. */
	for (i = 0; i < n_boards; i++) {
		boards[i].ut = ubertooth_init();
		if (boards[i].ut == NULL) {
			/* libubertooth has said what it could not allocate.
			 * Whatever was allocated before it goes back, so a run
			 * that ends here leaves nothing behind. */
			rx_give_back_boards(boards, n_boards, 0);
			return 1;
		}
	}
	ut = boards[0].ut;

	/* And the replay's input can be opened: a refused command line creates
	 * and truncates nothing. It is read, not written, so it comes before
	 * the board; the files this run writes are opened further down, once
	 * the board is known to be usable. */
	if (infile_name != NULL) {
		infile = fopen(infile_name, "r");
		if (infile == NULL) {
			/* A file that is not there is a runtime failure, not a
			 * bad command line, so it gets no usage text; same
			 * shape as the other file-open failures. */
			fprintf(stderr, "error: %s: %s\n",
			        infile_name, strerror(errno));
			rx_give_back_boards(boards, n_boards, 0);
			return 1;
		}
	}
	/* Stop on Ctrl-C from here on, before the board is opened: a signal
	 * that arrives while one is being found would otherwise be discarded.
	 * A replay gets the handlers as well, so an interrupted one ends where
	 * it was asked to and exits 0 with what it had decoded, instead of
	 * being killed by the signal -- or, started as a background job by a
	 * script, reading on to the end of the file. */
	ubertooth_catch_stop_signals();

	if (infile == NULL) {
		/* Every board named, in turn. One that cannot be opened or
		 * whose firmware this host will not drive ends the run: a
		 * capture spread over fewer boards than were asked for would
		 * leave channels unheard with nothing saying which. */
		for (i = 0; i < n_boards; i++) {
			r = ubertooth_connect_select(boards[i].ut,
			                             selectors[i]);
			if (r < 0) {
				/* The library has closed whatever it opened;
				 * the close here is what ends the libusb
				 * session it started. */
				rx_give_back_boards(boards, n_boards, 0);
				return ubertooth_open_stopped() ? 0 : 1;
			}

			r = ubertooth_check_api(boards[i].ut);
			if (r < 0) {
				/* A refused version was released inside
				 * ubertooth_check_api(); one it could not read
				 * at all leaves the board open. Closing covers
				 * both, because closing a board already
				 * released does nothing, and it sends no STOP
				 * to a firmware this host will not drive. */
				rx_give_back_boards(boards, n_boards, 0);
				return 1;
			}

			/* Stopped while a board was being opened: the run ends
			 * here, with every board given back and no capture file
			 * left behind, rather than starting a mode and stopping
			 * it again. */
			if (ubertooth_stop_requested()) {
				rx_give_back_boards(boards, n_boards, 0);
				return 0;
			}
		}
	}

	/* libbtbb refuses only a -e out of its own range, which the check on -e
	 * above has already caught, and it says so itself. The status is 1, as
	 * every failure here is: the failing call's own return value used to go
	 * out as the exit status, which a shell wraps into a byte (-9 read as
	 * 247), so a script could not tell a failure from anything else. */
	r = btbb_init(max_ac_errors);
	if (r < 0) {
		/* A capture has a board open, in whatever mode an earlier
		 * command left it, so it is given back here rather than left
		 * claimed, as ubertooth-scan does on the same failure. A replay
		 * opened none, and the stop then does nothing. */
		rx_give_back_boards(boards, n_boards, 1);
		return 1;
	}

	if(survey_mode) {
		// auto-flush stdout so that wrapper scripts work
		setvbuf(stdout, NULL, _IONBF, 0);
		/* One survey for every board. The table libbtbb keeps per
		 * address is the process's, not a board's, and the receive loop
		 * below is single-threaded, so every board's packets reach the
		 * one survey and each address is heard as many times faster as
		 * there are boards. */
		btbb_init_survey();
	} else {
		if (have_lap) {
			/* A piconet per board: see struct rx_board. */
			for (i = 0; i < n_boards; i++) {
				boards[i].pn = btbb_piconet_new();
				if (boards[i].pn == NULL) {
					/* Out of memory, which the next line
					 * would dereference. A capture with no
					 * piconet to follow has nothing to do,
					 * so the run ends here, and the STOP
					 * covers a mode an earlier command left
					 * running. */
					fputs("error: out of memory\n", stderr);
					rx_give_back_boards(boards, n_boards, 1);
					return 1;
				}
				btbb_init_piconet(boards[i].pn, lap);
				if (have_uap)
					btbb_piconet_set_uap(boards[i].pn, uap);
			}
			pn = boards[0].pn;
			/* A board that refused this would follow whatever
			 * address it was left on, so the capture does not
			 * start. It is sent here, with the rest of the setup
			 * that can fail, so a board that refuses it leaves no
			 * empty capture behind either. A replay sends no
			 * request at all. */
			if (have_uap && infile == NULL) {
				for (i = 0; i < n_boards; i++) {
					r = cmd_set_bdaddr_quiet(
					        boards[i].ut->devh,
					        btbb_piconet_get_bdaddr(pn));
					if (r < 0)
						return request_failed(boards,
						        n_boards,
						        "set the Bluetooth address",
						        r);
				}
			}
		}
	}

	files.pcapng = pcapng_name;
	files.pcap = pcap_name;
	files.dump = dump_name;

	/* The board is open and usable, or this is a replay, and every step of
	 * the setup that can fail is behind us, so the files this run writes
	 * can be opened. Not before: a run refused for its firmware, or one
	 * libbtbb or the board would not set up, used to leave an empty capture
	 * behind, which a user then had to tell apart from a capture that
	 * caught nothing.
	 *
	 * A lone "-" redirects this process's standard output, which is why
	 * nothing may be printed there after this point except the capture
	 * itself, and why the per-packet report goes to report_stream. */
	errno = 0;
	if (pcapng_name != NULL &&
	    btbb_pcapng_create_file(pcapng_name, "Ubertooth", &ut->h_pcapng_bredr)) {
		opt_report_create_failure(pcapng_name, errno);
		return setup_failed(boards, n_boards, &files);
	}
	if (pcap_name != NULL) {
		/* A lone "-" is standard output, which the capture then owns:
		 * see opt_capture_path(). The per-packet report this tool
		 * prints there goes nowhere for such a run, which is what the
		 * man page says of it; the notes and failures are on standard
		 * error and still arrive. */
		int saved_fd;
		char *file = opt_capture_path(pcap_name, "binary capture data", &saved_fd);
		int failed, create_errno;

		if (file == NULL) {
			if (saved_fd >= 0)
				close(saved_fd);
			return setup_failed(boards, n_boards, &files);
		}
		errno = 0;
		failed = btbb_pcap_create_file(file, &ut->h_pcap_bredr);
		/* Read now: the close() and the free() below may set an errno
		 * of their own, and this one is the reason reported. */
		create_errno = errno;
		if (saved_fd >= 0) {
			/* The capture has standard output, which is /dev/null for
			 * this process from here on. */
			report_stream = stderr;
			close(saved_fd);
		}
		free(file);
		if (failed) {
			/* The name the user gave, not the /dev/fd path a lone
			 * "-" is opened through. */
			opt_report_create_failure(pcap_name, create_errno);
			return setup_failed(boards, n_boards, &files);
		}
	}
	if (dump_name != NULL) {
		dumpfile = opt_open_dump_file(dump_name, "binary dump data");
		if (dumpfile != NULL && strcmp(dump_name, "-") == 0)
			report_stream = stderr;
		if (dumpfile == NULL) {
			/* The capture files are on the handle, so the close is
			 * what gets them written out and shut, and it gives the
			 * board back. The files this run created are taken away
			 * first, so the write lands in a file already gone. */
			return setup_failed(boards, n_boards, &files);
		}
	}

	/* The address being followed, recorded once so a reader of the capture
	 * knows whose packets these are. It needs the PcapNG file, so it is the
	 * one part of the setup above that has to wait for it. Reported when it
	 * cannot be written: the capture goes on either way, but without this
	 * the packets in it have nothing to say which piconet they belong to.
	 * A piconet exists only where -l named one: a survey has no single
	 * address to record. */
	if (pn != NULL && ut->h_pcapng_bredr) {
		if (btbb_pcapng_record_bdaddr(ut->h_pcapng_bredr,
		        (((uint32_t)uap)<<24)|lap,
		        have_uap ? 0xff : 0x00, 0) < 0)
			fprintf(stderr, "warning: cannot record the target "
			        "address in the capture file\n");
	}

	/* One capture file, however many boards write into it: the writers are
	 * on the first board's handle and the rest are pointed at the same
	 * ones, because the callback writes through the handle the packet came
	 * from. rx_unshare_files() takes the copies off again before the
	 * teardown, so the file is closed once. */
	for (i = 1; i < n_boards; i++) {
		boards[i].ut->h_pcapng_bredr = ut->h_pcapng_bredr;
		boards[i].ut->h_pcap_bredr = ut->h_pcap_bredr;
	}

	if (infile == NULL) {
		/* One timer for the run, not one per board: it is an alarm, and
		 * it stops whichever capture is running. */
		if (timeout)
			ubertooth_set_timeout(ut, timeout);

		// init USB transfer
		for (i = 0; i < n_boards; i++) {
			r = ubertooth_bulk_init(boards[i].ut);
			if (r < 0) {
				/* nothing is in flight, but the board is still open and in
				 * whatever mode an earlier command left it. libubertooth has
				 * already said what failed, so the status is all this adds. */
				return transfer_failed(boards, n_boards, &files);
			}
		}

		r = ubertooth_bulk_thread_start();
		if (r != 0) {
			/* Not "< 0": this is pthread_create()'s errno, which is
			 * positive, so a failure used to slip through and leave the
			 * capture with nobody to complete its transfers -- it then
			 * waited forever with nothing arriving. */
			report_capture_thread_failure(r);
			/* The receive transfer is in flight and the board is about to
			 * be left in its mode: give both back before returning. */
			return transfer_failed(boards, n_boards, &files);
		}

		/* Every board in turn, with all three requests sent to one
		 * board before the next is set up: they belong together,
		 * because the mode start between them is what makes a board
		 * forget its channel. */
		for (i = 0; i < n_boards; i++) {
			/* The modulation this capture reads, Classic, stated
			 * before the capture request and not after it as the
			 * channel is: see ubertooth-util(1), CARRY-OVER BETWEEN
			 * RUNS. Without it, a board an interrupted
			 * `ubertooth-dump -l` had left set to low energy
			 * captured here at that deviation, finding little and
			 * saying nothing. */
			r = cmd_set_modulation_quiet(boards[i].ut->devh,
			                             MOD_BT_BASIC_RATE);
			if (r < 0)
				return capture_failed(boards, n_boards, &files,
				                      "set the modulation", r);

			// tell ubertooth to send packets
			/* The quiet form, so this failure reads as the one "error: " line
			 * capture_failed() prints, like the channel request below. The
			 * reporting form would hand it to libubertooth's shared sink
			 * instead, which does not name the request that failed. */
			r = cmd_rx_syms_quiet(boards[i].ut->devh);
			if (r < 0)
				return capture_failed(boards, n_boards, &files,
				                      "start the capture", r);

			/* Tell the board how to tune for this capture after asking for
			 * the capture, not before. Starting a mode puts the board's hop
			 * mode back to "no hopping", so a sweep stated first is thrown
			 * away: the capture then ran on one channel while the user had
			 * asked for the whole band, and nothing said so.
			 *
			 * The board keeps what arrives after the request for the mode it
			 * is starting, whatever it was doing before. A board left in a
			 * mode that captures nothing -- a killed ubertooth-specan say --
			 * used to lose this request, because it runs the reset a moment
			 * after this one has been answered, on the way out of that mode.
			 *
			 * The board stays on whatever it was tuned to until this
			 * request arrives and the next hop acts on it, which takes up
			 * to 40 ms. Blocks captured in that window carry the channel
			 * they were captured on, as every block does, so they are
			 * reported for what they are rather than mislabelled.
			 *
			 * A board that refused this would capture on a channel the user
			 * did not ask for and say nothing about it, so the run ends
			 * instead. */
			r = cmd_set_channel_quiet(boards[i].ut->devh,
			                          boards[i].channel);
			if (r < 0)
				return capture_failed(boards, n_boards, &files,
				                      have_channel ? "set the channel"
				                                   : "set sweep mode", r);
			/* Kept for the callback, which starts a mode again when the
			 * library gives up a clock and has to restate this. */
			boards[i].ut->capture_channel = boards[i].channel;
		}

		// receive and process each packet
		/* Round robin over the boards. One loop and one thread, so
		 * libbtbb is called once at a time whatever it is reading: that
		 * is what lets several boards feed one survey, and what the
		 * library's threading rules ask for. */
		while(!rx_stopping(boards, n_boards)) {
			/* Whether any board handed the decoder a record this
			 * time round, so the merge below runs when there is
			 * something new to merge rather than on every spin of
			 * a loop that is mostly waiting. */
			int decoded = 0;

			for (i = 0; i < n_boards; i++) {
				/* A follow that locked onto the wrong clock hears next
				 * to nothing rather than failing, so nothing in the
				 * packets that do arrive reports it: the clock is given
				 * up on the wait above instead. A negative reading is a
				 * capture following no clock the library gave it, and
				 * the drop starts the wait again whatever comes of it,
				 * so a follow that stays quiet asks once per wait.
				 * Before the receive, so a capture that has just been
				 * stopped sends no request on its way out.
				 *
				 * Per board: each has its own clock from the
				 * library and hears the piconet for itself. */
				if (ubertooth_follow_quiet_ms(boards[i].ut)
				    >= FOLLOW_QUIET_MS)
					ubertooth_follow_drop_clock(boards[i].ut,
					                            boards[i].pn);
				/* And what the search is not going to reach,
				 * said once a search has been running long
				 * enough to say it of. */
				rx_note_clock_search_limit(boards[i].ut);
				if (ubertooth_bulk_receive(boards[i].ut, cb_rx,
				                           boards[i].pn) >= 0)
					decoded = 1;
			}
			/* What one board has learned, handed to the others. */
			if (have_lap && decoded)
				rx_share_findings(boards, n_boards, merged_map);
		}

		ubertooth_bulk_thread_stop();

		/* The capture file goes back to the one handle that will write
		 * it out, before any board is torn down. */
		rx_unshare_files(boards, n_boards);

		for (i = 0; i < n_boards; i++) {
			ubertooth_stop(boards[i].ut);

			/* What the capture is missing, once the packets have
			 * scrolled past. The counts are of the places a loss
			 * was marked, not of the packets lost at them, and each
			 * board counts its own. */
			ubertooth_report_capture_losses(boards[i].ut);

			/* A capture whose stream died stopped there, so it holds less than
			 * was asked for. The exit status has to say so, or a caller cannot
			 * tell it from a timeout or a Ctrl-C. The handle is read after the
			 * teardown, because cancelling the transfer can be what brings the
			 * failure out. The survey results below are still printed, since
			 * they are what did arrive, and libubertooth has already printed
			 * what failed.
			 *
			 * A teardown the board refused counts the same way: that board
			 * is left running the mode this capture started, and no other
			 * tool can open it until it is reset or re-plugged. */
			if (ubertooth_stream_failed(boards[i].ut) ||
			    ubertooth_stop_failed(boards[i].ut))
				exit_status = 1;
		}

		/* What the boards found between them. Only where there was
		 * something to merge: one board's result is its own piconet's,
		 * and libbtbb reports that as it goes. */
		if (have_lap && n_boards > 1)
			rx_report_findings(n_boards, merged_map, boards,
			                   have_uap);
	} else {
		/* A read error or a last record cut short is reported and ends
		 * the command with an error, after whatever was decoded. */
		r = stream_rx_file(ut, infile, replay_decode, pn);
		fclose(infile);
		exit_status = rx_file_report(r, infile_name);

		/* An input no whole record could be read from leaves the
		 * capture files holding their headers and nothing else, so
		 * they go the way a refused capture's files do. From the first
		 * record on they stay, however little of the input came after
		 * it. An input read to its end is no failure, an empty one
		 * included, and never reaches here: that replay ran. */
		if (exit_status != 0 && !replay_read_a_record)
			rx_discard_files(ut, &files);

		/* A recorded capture carries the losses it was recorded with, in
		 * the status of its own records, and the replay is where a reader
		 * of the file finds out about them. */
		ubertooth_report_capture_losses(ut);
	}

	if(survey_mode) {
		/* The heading is for a person reading along, not part of the
		 * result list, so it goes to standard error and leaves standard
		 * output holding only the addresses a script parses. The same
		 * split ubertooth-scan(1) prints "Scan results:" with. */
		fputs("Survey Results\n", stderr);
		while((pn=btbb_next_survey_result()) != NULL) {
			lap = btbb_piconet_get_lap(pn);
			if (btbb_piconet_get_flag(pn, BTBB_UAP_VALID)) {
				uap = btbb_piconet_get_uap(pn);
				/* Printable version showing that the NAP is unknown */
				fprintf(report_stream, "??:??:%02X:%02X:%02X:%02X\n", uap,
						(lap >> 16) & 0xFF, (lap >> 8) & 0xFF, lap & 0xFF);
			} else
				fprintf(report_stream, "??:??:??:%02X:%02X:%02X\n",
					   (lap >> 16) & 0xFF, (lap >> 8) & 0xFF, lap & 0xFF);
			//btbb_print_afh_map(pn);
			/* The walk hands the reference over, so each piconet
			 * is released once it has been reported. */
			btbb_piconet_unref(pn);
		}
		pn = NULL;
	}
	if(dumpfile != NULL)
		fclose(dumpfile);
	/* The capture files the first board's handle holds, for a replay as for
	 * a capture: the close does nothing to a handle that opened no board,
	 * and a board already stopped above is not stopped twice. */
	rx_give_back_boards(boards, n_boards, 0);

	return exit_status;
}
