# The tree's release-or-development setting does not come into the tests: a
# case that cares compiles the library source itself with the value it wants,
# or asks for the behaviour through the environment, so no test result depends
# on whether this checkout happens to sit on a tag.
remove_definitions(-DUBERTOOTH_RELEASE_BUILD=${UBERTOOTH_RELEASE_BUILD})

# Most targets here compile a libubertooth source that reaches
# ubertooth_control.h, which includes <libusb-1.0/libusb.h>, so libusb's
# include directories are given to the whole directory, as the library's and
# the tools' own CMakeLists.txt do for theirs. The targets that need no libusb
# header are unharmed by having the directories, and a target added later
# needs nothing: naming them one target at a time is what let a new target
# arrive with libbtbb's directory and not libusb's, which on a Homebrew
# install compiles by coincidence.
#
# This directory asks for libusb itself: the directories are a normal
# variable, and src/ is a sibling rather than a parent, so neither its
# find_package() nor its include_directories() reaches here.
find_package(USB1 REQUIRED)
if(USE_OWN_GNU_GETOPT)
	# Ahead of libusb's directories, which on a Homebrew install are a
	# prefix include directory holding whatever else is installed there: a
	# getopt.h among it must not be taken for this project's own. The
	# targets that link opt-parse-for-tests get this directory from it as
	# well, but behind the ones granted here.
	include_directories(BEFORE ../getopt)
endif()
include_directories(${LIBUSB_INCLUDE_DIRS})

# Given to the whole directory for the same reason as those include
# directories. Every target that compiles ubertooth.c needs it: the host
# version report calls dladdr(), which glibc kept in libdl until 2.34 and has
# had in libc since, so a target that compiles the source instead of linking
# libubertooth has to ask for it, and the targets that do not need it are
# unharmed. The variable is empty where libc has dladdr(), which is why only
# the older distributions failed to link, and only on their full-tier job --
# naming them one target at a time is how the next such target would get
# through a pull request and break that job again.
link_libraries(${CMAKE_DL_LIBS})

add_executable(test-dfu-transition
	test_dfu_transition.c
	../src/dfu_transition.c)

target_include_directories(test-dfu-transition PRIVATE ../src)

add_test(NAME dfu-transition COMMAND test-dfu-transition)

add_executable(test-dfu-recovery
	test_dfu_recovery.c
	../src/dfu_recovery.c
	../src/dfu_transition.c)

# dfu_recovery.h describes the firmware info block, whose layout is shared
# with the firmware in libubertooth's ubertooth_interface.h.
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-dfu-recovery PRIVATE ${libubertooth_SOURCE_DIR}/src)
else()
	target_include_directories(test-dfu-recovery PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-dfu-recovery PRIVATE ../src)

add_test(NAME dfu-recovery COMMAND test-dfu-recovery)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ubertooth-dfu checks the image before it opens a device, so the whole
# accept/refuse decision runs against the real binary with no board. Fifteen
# short runs, well under a second in total now that the test hides attached
# boards from the tool, so the timeout only catches a tool that waits for a
# device it should never have looked for.
add_test(
	NAME dfu-images
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_dfu_images.py
		$<TARGET_FILE:ubertooth-dfu>)
# About 0.3 s idle, 1.2 s loaded. A minute leaves the test's own twenty-second
# per-run deadline room to fire first and name the run that timed out.
set_tests_properties(dfu-images PROPERTIES TIMEOUT 60)

# ubertooth-dfu's file handling against the real binary: -s, which files -d
# and -u open or replace, and the files -d and -s refuse to read whole. Needs
# no board, and opens none: the cases that would reach one use a serial no
# board has, with attached boards hidden from the tool. About thirty-seven
# short runs, so the timeout only catches a tool that waits for a device.
add_test(
	NAME dfu-files
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_dfu_files.py
		$<TARGET_FILE:ubertooth-dfu>)
# About 0.6 s idle, 1.3 s loaded. A minute leaves the test's own twenty-second
# per-run deadline room to fire first and name the run that timed out.
set_tests_properties(dfu-files PROPERTIES TIMEOUT 60)

add_test(
	NAME ducky-options
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_ducky_options.py
		$<TARGET_FILE:ubertooth-ducky>)

# The shared option parsing, built once for the tests that need it, with the
# same getopt the tools use (opt_parse.c reads getopt's optind).
add_library(opt-parse-for-tests STATIC ../src/opt_parse.c)
target_include_directories(opt-parse-for-tests PUBLIC ../src)
if(USE_OWN_GNU_GETOPT)
	target_include_directories(opt-parse-for-tests BEFORE PUBLIC ../getopt)
	target_link_libraries(opt-parse-for-tests PUBLIC libgetopt_static)
endif()

add_executable(test-opt-parse test_opt_parse.c)
target_link_libraries(test-opt-parse opt-parse-for-tests)

add_test(NAME opt-parse COMMAND test-opt-parse)

add_executable(test-specan-options
	test_specan_options.c
	../src/specan_options.c)

target_include_directories(test-specan-options PRIVATE ../src)
target_link_libraries(test-specan-options opt-parse-for-tests)

add_test(NAME specan-options COMMAND test-specan-options)

# Putting a long low-energy packet back together from the records it arrives
# in. Pure logic over the record format, so it needs neither libusb nor
# libbtbb: only the shared interface header the state is sized from.
#
# It compiles the library's own ubertooth_le_frag.c, which only a source tree
# has: an installed libubertooth ships the header and the shared object, not
# the source. So this case is built only when the tools are configured
# alongside the library, and skipped when they are configured against an
# installed one.
if(libubertooth_SOURCE_DIR)
	add_executable(test-le-frag
		test_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c)
	target_include_directories(test-le-frag PRIVATE ${libubertooth_SOURCE_DIR}/src)

	add_test(NAME le-frag COMMAND test-le-frag)
	# Arithmetic and memcpy over a few hundred bytes: milliseconds.
	set_tests_properties(le-frag PROPERTIES TIMEOUT 5)
endif()

# What a specan run does when it cannot write its samples, through the
# callback and through its real main(). cb_specan() is static in
# ubertooth-specan.c, so that source is compiled in with main() renamed, as
# test_util_main.c does. The library's device functions are the fakes in the
# test; only its fifo is the real one, so no device is opened.
if(libubertooth_SOURCE_DIR)
	# request_report.c is the real one, so the cases pin the wording the
	# tool prints rather than a stub's.
	add_executable(test-specan-write-failure
		test_specan_write_failure.c
		../src/specan_options.c
		../src/request_report.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_fifo.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_include_directories(test-specan-write-failure PRIVATE
		../src ${libubertooth_SOURCE_DIR}/src
		${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-specan-write-failure opt-parse-for-tests)
	if(USE_OWN_GNU_GETOPT)
		target_include_directories(test-specan-write-failure BEFORE PRIVATE ../getopt)
		target_link_libraries(test-specan-write-failure libgetopt_static)
	endif()

	add_test(NAME specan-write-failure COMMAND test-specan-write-failure)
	# A handful of callback calls and short main() runs against a scripted
	# device: a tenth of a second. The limit is loose enough for the two
	# dozen temp files the cases make on a loaded machine, and still fails a
	# regression that reached a real receive loop rather than hanging the
	# suite.
	set_tests_properties(specan-write-failure PROPERTIES TIMEOUT 10)
endif()

# The one argument rule, per tool, run against the real binaries with no
# device: one CTest entry per built tool so a failure names the tool. Each
# case is a single short run; the timeout only catches a tool that hangs
# instead of refusing (a whole tool's cases take well under a second).
foreach(tool ${UBERTOOTH_TOOLS})
	add_test(
		NAME cli-arguments-${tool}
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_cli_arguments.py
			$<TARGET_FILE:${tool}>)
	set_tests_properties(cli-arguments-${tool} PROPERTIES TIMEOUT 60)
	add_test(
		NAME board-not-found-${tool}
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_board_not_found.py
			$<TARGET_FILE:${tool}>)
	set_tests_properties(board-not-found-${tool} PROPERTIES TIMEOUT 30)
	# A refused command line must create and truncate nothing: every file
	# option is opened only after the whole command line is accepted. One
	# entry per built tool so a failure names the tool; the tools with no
	# file option say so and pass.
	add_test(
		NAME no-file-on-refusal-${tool}
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_no_file_on_refusal.py
			$<TARGET_FILE:${tool}>)
	set_tests_properties(no-file-on-refusal-${tool} PROPERTIES TIMEOUT 30)
	# Which stream the usage text lands on, and the exit status, for -h and
	# for a command line the tool will not take. One entry per built tool.
	add_test(
		NAME output-streams-${tool}
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_output_streams.py
			$<TARGET_FILE:${tool}>)
	# About 0.2 s per tool idle, 0.3 s loaded. A minute leaves the test's
	# own twenty-second per-run deadline room to fire first and name the
	# run that timed out.
	set_tests_properties(output-streams-${tool} PROPERTIES TIMEOUT 60)
	# The whole command-line contract of the tool, generated from
	# cli_spec.json: every option alone, every bad value, every pair, and
	# the stream and status rules for each. One entry per built tool so a
	# failure names the tool. Several hundred short runs per tool, so the
	# timeout only catches a tool that waits for a device it should never
	# have looked for; ubertooth-util, with the most options, is the
	# slowest at 684 runs: 21 s on macOS, where starting a process is dear,
	# and 0.8 s on the Linux runners. The limit is generous against that
	# because each run inside the test has its own 20 s deadline, and the
	# per-case report is only printed when the test itself finishes.
	add_test(
		NAME cli-matrix-${tool}
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_cli_matrix.py
			${CMAKE_CURRENT_SOURCE_DIR}/cli_spec.json
			$<TARGET_FILE:${tool}>)
	set_tests_properties(cli-matrix-${tool} PROPERTIES TIMEOUT 180)
endforeach()

# The same contract for btle-extcap.py, which has a command line of its own:
# the extcap protocol Wireshark calls it with, all long options. The script
# sits outside this directory, so it is named by a relative path and the entry
# is registered only when it is there: host/ubertooth-tools can be configured
# on its own.
set(UBERTOOTH_BTLE_EXTCAP
	"${CMAKE_CURRENT_SOURCE_DIR}/../../python/extcap/btle-extcap.py")
if(EXISTS "${UBERTOOTH_BTLE_EXTCAP}")
	add_test(
		NAME cli-matrix-btle-extcap
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_cli_matrix.py
			${CMAKE_CURRENT_SOURCE_DIR}/cli_spec.json
			${UBERTOOTH_BTLE_EXTCAP}
			# The script answers -V by running ubertooth-util, so the one
			# from this build goes first on the PATH of every run.
			$<TARGET_FILE:ubertooth-util>)
	set_tests_properties(cli-matrix-btle-extcap PROPERTIES TIMEOUT 60)
endif()

# install.sh answers -h on standard output, refuses a bad command line on
# standard error, and announces each of its slow steps before running it. The
# test installs nothing and touches no device: the runs that get that far have
# brew, python3, git and the Ubertooth tools faked. The script sits above the
# host build, so it is named by a relative path and the test is registered only
# when it is actually there: host/ can be configured on its own.
set(UBERTOOTH_INSTALL_SH "${CMAKE_CURRENT_SOURCE_DIR}/../../../install.sh")
if(EXISTS "${UBERTOOTH_INSTALL_SH}")
	add_test(
		NAME install-streams
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_install_streams.py
			${UBERTOOTH_INSTALL_SH})
	set_tests_properties(install-streams PROPERTIES TIMEOUT 120)
	# install.sh parses ubertooth-util's device listing, so the lines it is
	# given here come from that tool's man page rather than from a fake of
	# its own: a change to the listing then fails here instead of leaving
	# every serial as "?".
	add_test(
		NAME install-devices
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_install_devices.py
			${UBERTOOTH_INSTALL_SH}
			${CMAKE_CURRENT_SOURCE_DIR}/../../doc/ubertooth-util.md)
	set_tests_properties(install-devices PROPERTIES TIMEOUT 60)
endif()

add_test(
	NAME specan-cli
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_specan_cli.py
		$<TARGET_FILE:ubertooth-specan>)

add_test(
	NAME btle-cli
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_btle_cli.py
		$<TARGET_FILE:ubertooth-btle>)

# What ubertooth-debug -r accepts and refuses: the whole value is read before
# a device is looked for, so both halves run against the real binary with no
# board, with attached boards hidden from the tool. Twenty-one short runs.
add_test(
	NAME debug-cli
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_debug_cli.py
		$<TARGET_FILE:ubertooth-debug>)
# About 0.4 s idle, 0.5 s loaded. A minute leaves the test's own twenty-second
# per-run deadline room to fire first and name the run that timed out.
set_tests_properties(debug-cli PROPERTIES TIMEOUT 60)

# The file replay path of ubertooth-rx (-i), on fixtures the test builds
# itself: the recorded times in the packet lines and in a capture written
# during the replay, and the exit status for an unreadable or truncated file.
# No device is involved; the handful of runs take well under a second.
add_test(
	NAME rx-replay
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_rx_replay.py
		$<TARGET_FILE:ubertooth-rx>)
# About 0.4 s on an idle machine and 1 s on one busy with parallel builds. The
# old thirty seconds matched the test's own per-replay deadline, so a loaded
# machine was cut off here with nothing said and a hang was never reported by
# the test. Ninety leaves that deadline room to fire first and say so.
set_tests_properties(rx-replay PROPERTIES TIMEOUT 90)

# What the capture records of a replay say about the piconet they came from:
# the reference UAP, the packet's own header, and which LAP is recorded where.
# The fixture is built by the test itself -- an ID packet, a DH1 whose CRC
# leaves every clock candidate alive, the DH1 whose CRC settles the UAP, then
# another ID packet -- and is replayed three times: followed, surveyed, and
# with neither option. Three runs, well under a second each.
add_test(
	NAME rx-uap-record
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_rx_uap_record.py
		$<TARGET_FILE:ubertooth-rx>)
# About 0.7 s on an idle machine and 1 s on one busy with parallel builds. The
# old ten seconds was below the test's own per-replay deadline, so a loaded
# machine was cut off here with nothing said and a hang was never reported by
# the test. A minute leaves that deadline room to fire first and say so.
set_tests_properties(rx-uap-record PROPERTIES TIMEOUT 60)

add_executable(test-btle-options
	test_btle_options.c
	../src/btle_options.c)

target_include_directories(test-btle-options PRIVATE ../src)
target_link_libraries(test-btle-options opt-parse-for-tests)

add_test(NAME btle-options COMMAND test-btle-options)

add_executable(test-ego-options
	test_ego_options.c
	../src/ego_options.c)

target_include_directories(test-ego-options PRIVATE ../src)
target_link_libraries(test-ego-options opt-parse-for-tests)

add_test(NAME ego-options COMMAND test-ego-options)

add_test(
	NAME ego-cli
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_ego_cli.py
		$<TARGET_FILE:ubertooth-ego>)

add_executable(test-util-report
	test_util_report.c
	../src/util_report.c)

# util_report.h describes the firmware info block -v and -V report and the
# watchdog breadcrumb, whose layouts are in libubertooth's
# ubertooth_interface.h.
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-util-report PRIVATE ${libubertooth_SOURCE_DIR}/src)
else()
	target_include_directories(test-util-report PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-util-report PRIVATE ../src)

add_test(NAME util-report COMMAND test-util-report)

add_executable(test-debug-registers
	test_debug_registers.c
	../src/debug_registers.c
	../src/cc2400.c)

target_include_directories(test-debug-registers PRIVATE ../src)

add_test(NAME debug-registers COMMAND test-debug-registers)

add_executable(test-util-modes
	test_util_modes.c
	../src/util_modes.c)

target_include_directories(test-util-modes PRIVATE ../src)

add_test(NAME util-modes COMMAND test-util-modes)

# What ubertooth-follow -a makes of a map read from a remote device. The
# module is free of BlueZ, so this builds and runs everywhere, including on
# macOS where ubertooth-follow itself is not built at all.
add_executable(test-follow-afh
	test_follow_afh.c
	../src/follow_afh.c)

target_include_directories(test-follow-afh PRIVATE ../src)

add_test(NAME follow-afh COMMAND test-follow-afh)
# Arithmetic and string building only; the retry waits are faked.
set_tests_properties(follow-afh PROPERTIES TIMEOUT 5)

add_executable(test-util-reset-wait
	test_util_reset_wait.c
	../src/util_reset_wait.c)

target_include_directories(test-util-reset-wait PRIVATE ../src)

add_test(NAME util-reset-wait COMMAND test-util-reset-wait)

# Runs ubertooth-util's real main() against a scripted fake device. The source
# is included directly (main renamed) so it needs the library headers, and the
# bundled getopt on platforms that use it.
# The clock is faked in the test (util_clock.c is not linked), so a reset wait
# runs instantly.
# ubertooth_api_version.c is the real library source, not a fake, so the cases
# pin the message -I actually prints; it only calls fprintf, so it opens no
# device. Against an installed libubertooth there is no source to compile in,
# and the test stubs the message instead.
add_executable(test-util-main
	test_util_main.c
	../src/util_report.c
	../src/util_modes.c
	../src/util_reset_wait.c)
target_link_libraries(test-util-main opt-parse-for-tests)

# The library headers of this source tree must win over any installed copy.
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-util-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
	target_sources(test-util-main PRIVATE
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_compile_definitions(test-util-main PRIVATE UTIL_TEST_REAL_API_MESSAGE)
else()
	target_include_directories(test-util-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
# -v and -V both print what libubertooth's ubertooth_version.c reads off the
# board, so that source is compiled in either way and the cases pin the lines
# the tool really prints. It sits beside the tools in this repository, so the
# path holds for a build against an installed library too.
target_sources(test-util-main PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../libubertooth/src/ubertooth_version.c)
target_include_directories(test-util-main PRIVATE ../src
	${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-util-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-util-main libgetopt_static)
endif()

add_test(NAME util-main COMMAND test-util-main)
# A regression that lets a mode such as -I (identify, blinks until
# interrupted) run inside main() would hang; fail it instead. A normal run
# takes well under a second (0.02-0.5 s locally and in CI).
set_tests_properties(util-main PROPERTIES TIMEOUT 5)

# The -V answer every tool gives, which is libubertooth's
# ubertooth_version.c: the real source, with the test's fakes for the device
# calls it makes, so the cases pin what a tool prints. Nothing else of the
# library is linked, so no device is opened. Skipped against an installed
# libubertooth, where there is no source to compile in.
if(libubertooth_SOURCE_DIR)
	add_executable(test-version-report
		test_version_report.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_version.c)
	target_include_directories(test-version-report PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIRS}
		${LIBBTBB_INCLUDE_DIR})
	add_test(NAME version-report COMMAND test-version-report)
	# A few lines into a temporary file, ten times over: milliseconds.
	set_tests_properties(version-report PROPERTIES TIMEOUT 5)
endif()

# run_main.h, the harness that runs a tool's main() with both its streams
# captured, for the tests below that drive one. Its own cases drive a fake
# main(), so no tool, no library and no getopt is linked in.
add_executable(test-run-main test_run_main.c)

add_test(NAME run-main COMMAND test-run-main)
# Seven runs of a fake main() that returns at once, so the timeout only
# catches a harness that waits on something.
set_tests_properties(run-main PROPERTIES TIMEOUT 10)

# The same approach for ubertooth-afh: its real main() against a scripted
# detection phase, so what it reports and exits with is checked without a
# board or a piconet. libbtbb is not linked: the test defines the piconet
# calls itself, so a run's allocations can be counted and every path can be
# required to give the piconet back.
add_executable(test-afh-main test_afh_main.c)
target_link_libraries(test-afh-main opt-parse-for-tests)
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-afh-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
else()
	target_include_directories(test-afh-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-afh-main PRIVATE ../src
	${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-afh-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-afh-main libgetopt_static)
endif()

add_test(NAME afh-main COMMAND test-afh-main)
# Nothing here waits on a device; a regression that reaches a real receive
# loop should fail instead of hanging the suite.
set_tests_properties(afh-main PROPERTIES TIMEOUT 10)

# The same approach for ubertooth-btle: its real main() against a scripted
# device, so which commands the options run, in which order, which stream each
# line lands on and what the tool exits with are all checked without a board.
# The capture creation is faked here too, so a "-q -" run writes no PCAP header
# into the captured stdout and no file is created anywhere.
add_executable(test-btle-main
	test_btle_main.c
	../src/btle_options.c)
target_link_libraries(test-btle-main opt-parse-for-tests ${LIBBTBB_LIBRARIES})
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-btle-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
else()
	target_include_directories(test-btle-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-btle-main PRIVATE ../src
	${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-btle-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-btle-main libgetopt_static)
endif()

# The same approach for ubertooth-rx: its real main() against a scripted
# library, so what a live capture exits with is checked without a board.
# libbtbb is the real one (the tool hands it the piconet and the survey).
# request_report.c is the real one, so the cases pin the wording the tool
# prints rather than a stub's.
add_executable(test-rx-main test_rx_main.c ../src/request_report.c)
target_link_libraries(test-rx-main opt-parse-for-tests ${LIBBTBB_LIBRARIES})
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-rx-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
	# The real report for a request the board refused, so the cases pin the
	# library's wording rather than a stub's.
	target_sources(test-rx-main PRIVATE
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_compile_definitions(test-rx-main PRIVATE TEST_REAL_API_REPORTS)
else()
	target_include_directories(test-rx-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-rx-main PRIVATE ../src
	${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-rx-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-rx-main libgetopt_static)
endif()

add_test(NAME rx-main COMMAND test-rx-main)
# The scripted library ends each capture after a few blocks; a regression that
# waits out a -t timer, or a real device, should fail instead of hanging the
# suite.
set_tests_properties(rx-main PROPERTIES TIMEOUT 10)

add_test(NAME btle-main COMMAND test-btle-main)
# The fake poll ends the capture loop at once; a regression that waits on a
# real device should fail instead of hanging the suite.
set_tests_properties(btle-main PROPERTIES TIMEOUT 10)

# The same approach for ubertooth-ducky: its real main() against a scripted
# device, so what a quack sends, and what a refused request reports and exits
# with, are checked without a board and without transmitting. The two-second
# advertising wait is faked in the test, so a quack runs instantly.
add_executable(test-ducky-main test_ducky_main.c)
target_link_libraries(test-ducky-main opt-parse-for-tests)
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-ducky-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
else()
	target_include_directories(test-ducky-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-ducky-main PRIVATE ../src
	${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-ducky-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-ducky-main libgetopt_static)
endif()

add_test(NAME ducky-main COMMAND test-ducky-main)
# Every device call is faked; a regression that reaches a real board or waits
# out a real quack should fail instead of hanging the suite.
set_tests_properties(ducky-main PROPERTIES TIMEOUT 10)

# The same approach for ubertooth-ego: its real main() against a scripted
# device, so which requests set a capture up, and what a refused one reports
# and exits with, are checked without a board and without transmitting.
add_executable(test-ego-main
	test_ego_main.c
	../src/ego_options.c)
target_link_libraries(test-ego-main opt-parse-for-tests)
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-ego-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
else()
	target_include_directories(test-ego-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-ego-main PRIVATE ../src
	${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-ego-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-ego-main libgetopt_static)
endif()

add_test(NAME ego-main COMMAND test-ego-main)
# The faked poll ends the capture loop at once; a regression that waits on a
# real board should fail instead of hanging the suite.
set_tests_properties(ego-main PROPERTIES TIMEOUT 10)

# The same approach for ubertooth-dfu: its real main() against a faked DFU
# transport, so a bootloader that stops answering and a read-back that fails
# part-way are testable without a board.
add_executable(test-dfu-main
	test_dfu_main.c
	../src/dfu_recovery.c
	../src/dfu_transition.c)
target_link_libraries(test-dfu-main opt-parse-for-tests)
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-dfu-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
	# The real report for a request the bootloader refused, so the cases pin
	# the library's wording rather than a stub's.
	target_sources(test-dfu-main PRIVATE
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_compile_definitions(test-dfu-main PRIVATE TEST_REAL_API_REPORTS)
else()
	target_include_directories(test-dfu-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-dfu-main PRIVATE ../src
	${LIBBTBB_INCLUDE_DIR})
target_link_libraries(test-dfu-main ${LIBUSB_LIBRARIES})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-dfu-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-dfu-main libgetopt_static)
endif()

add_test(NAME dfu-main COMMAND test-dfu-main)
# The unbounded bootloader state loop this test covers would hang here; fail
# it instead. The transfers are faked and so are the waits between polls, so
# a normal run is well under a second.
set_tests_properties(dfu-main PROPERTIES TIMEOUT 5)

# Runs the real libubertooth part-ID commands against a fake libusb control
# transfer (defined in the test, so it wins over libusb's). libbtbb is faked;
# libusb is linked for the other symbols. Needs the library sources, so only
# when built from the whole host tree.
if(libubertooth_SOURCE_DIR)
	# ubertooth.c runs the capture ring buffer on its own thread, so every
	# test that compiles it needs pthread on the link line, whether or not
	# the case reaches the receive loop: glibc before 2.34 keeps the pthread
	# calls out of libc, and Ubuntu 20.04 (this fork's minimum) is one such.
	# Inside this guard, so a standalone ubertooth-tools configure, which
	# builds none of these, does not require it.
	find_package(Threads REQUIRED)

	# What the host does about firmware of another API version. The decision
	# differs between a release build and a build from a working checkout, so
	# the real library source is compiled once for each, and each copy
	# asserts what its own build should do.
	foreach(build release development)
		if(build STREQUAL release)
			set(release_build 1)
		else()
			set(release_build 0)
		endif()
		add_executable(test-api-mismatch-${build}
			test_api_mismatch.c
			${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
		target_include_directories(test-api-mismatch-${build} PRIVATE
			${libubertooth_SOURCE_DIR}/src
			${LIBBTBB_INCLUDE_DIR})
		target_compile_definitions(test-api-mismatch-${build} PRIVATE
			UBERTOOTH_RELEASE_BUILD=${release_build})

		add_test(NAME api-mismatch-${build}
			COMMAND test-api-mismatch-${build})
		# Formatting a few lines into a temporary file: milliseconds.
		set_tests_properties(api-mismatch-${build} PROPERTIES TIMEOUT 5)
	endforeach()

	add_executable(test-control-partnum
		test_control_partnum.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_include_directories(test-control-partnum PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-control-partnum ${LIBUSB_LIBRARIES})
	add_test(NAME control-partnum COMMAND test-control-partnum)

	# The same approach for the fixed-size read commands (short replies).
	add_executable(test-control-reads
		test_control_reads.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_include_directories(test-control-reads PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-control-reads ${LIBUSB_LIBRARIES})
	add_test(NAME control-reads COMMAND test-control-reads)

	# The same approach for the asynchronous commands (follow mode), with the
	# three libusb transfer calls faked in the test: what the library hands
	# libusb, and what it frees, without a board.
	add_executable(test-control-async
		test_control_async.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_include_directories(test-control-async PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-control-async ${LIBUSB_LIBRARIES})
	add_test(NAME control-async COMMAND test-control-async)
	# Every transfer is faked: a run takes milliseconds.
	set_tests_properties(control-async PROPERTIES TIMEOUT 5)

	# What ubertooth_init() does when the ring buffer cannot be allocated.
	# ubertooth_fifo.c is left out so the test can define the fifo calls and
	# fail the allocation on demand.
	add_executable(test-init-failure
		test_init_failure.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-init-failure PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-init-failure ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME init-failure COMMAND test-init-failure)
	# Nothing here waits on a device; a regression that reaches USB should
	# fail instead of hanging the suite.
	set_tests_properties(init-failure PROPERTIES TIMEOUT 10)

	# The same approach for the firmware revision and compile info commands.
	add_executable(test-control-text
		test_control_text.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_include_directories(test-control-text PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-control-text ${LIBUSB_LIBRARIES})
	add_test(NAME control-text COMMAND test-control-text)

	# Runs the real libubertooth device selection against a faked USB device
	# list (the libusb device calls are defined in the test), so the
	# multi-device -U cases are covered without attaching several boards.
	# ubertooth_fifo.c is left out so the test can define the fifo calls: a
	# real ring buffer is 72 MB per selection, allocated several times per
	# case for a buffer no case reads a packet from.
	add_executable(test-device-select
		test_device_select.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-device-select PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-device-select ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME device-select COMMAND test-device-select)
	# Nothing here waits on hardware; a regression that blocks on a real USB
	# call should fail instead of hanging the suite.
	set_tests_properties(device-select PROPERTIES TIMEOUT 10)

	# The quiet selection the reset wait polls with, against its own faked
	# USB bus: it must print nothing on either stream and must not send STOP
	# to a board it cannot claim. ubertooth_fifo.c is left out for the same
	# reason as above.
	add_executable(test-select-quiet
		test_select_quiet.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-select-quiet PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-select-quiet ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME select-quiet COMMAND test-select-quiet)
	# Every USB call is faked; a regression that reaches a real bus should
	# fail instead of hanging the suite.
	set_tests_properties(select-quiet PROPERTIES TIMEOUT 10)

	# The control requests the AFH paths send, in order, against the same
	# faked libusb: AFH detection has to run in the board's AFH mode, so
	# UBERTOOTH_RX_SYMBOLS must never be sent on top of it. The fifo calls
	# are faked in the test (ubertooth_fifo.c is not linked), which is also
	# what stops the receive loops.
	add_executable(test-afh-requests
		test_afh_requests.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-afh-requests PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-afh-requests ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME afh-requests COMMAND test-afh-requests)
	# The loops are stopped from the faked fifo after a few turns and every
	# transfer is faked, so a run takes milliseconds. A regression that leaves
	# a receive loop running would hang here; fail it instead.
	set_tests_properties(afh-requests PROPERTIES TIMEOUT 10)

	# The capture clock across its wrap. The time reconstruction is static in
	# ubertooth_callback.c, so the test includes that source itself; the rest
	# of the library is linked for the symbols the callbacks reference.
	add_executable(test-clk100ns-wrap
		test_clk100ns_wrap.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_fifo.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c)
	target_include_directories(test-clk100ns-wrap PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-clk100ns-wrap ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	# One case reads ubertooth-btle's source, which starts the mode whose
	# packets come from the other clock (like test_mode_dispatch does).
	target_compile_definitions(test-clk100ns-wrap PRIVATE
		BTLE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../src/ubertooth-btle.c")
	add_test(NAME clk100ns-wrap COMMAND test-clk100ns-wrap)
	# Arithmetic, one source read, a stop on a handle that never opened a
	# device, and a few runs that write a dump and a capture into a temporary
	# directory: a run takes well under a second.
	set_tests_properties(clk100ns-wrap PROPERTIES TIMEOUT 5)

	# The ring buffer itself, including the two threads that share it. Only
	# ubertooth_fifo.c is linked, so the ring can be built small: the
	# shipped 1000000 slots put a full ring out of a test's reach, and
	# every full-ring path is on the other side of it. Seven slots, not
	# eight: a size that is not a power of two, so replacing the ring's
	# wrap with a bitmask -- which would work at every size the test used
	# and at none that ships -- fails here. One variable for it, because
	# the sanitized target below has to agree.
	set(fifo_test_slots 7)
	#
	# No libbtbb include directory here, because nothing in this target
	# uses libbtbb: FindUSB1.cmake reports every directory libusb's
	# header needs, including the one <libusb-1.0/libusb.h> is found
	# under, so libusb's path no longer arrives by way of another
	# library's.
	add_executable(test-fifo-ring
		test_fifo_ring.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_fifo.c)
	target_include_directories(test-fifo-ring PRIVATE
		${libubertooth_SOURCE_DIR}/src)
	target_compile_definitions(test-fifo-ring PRIVATE
		UBERTOOTH_FIFO_SLOTS_FOR_TESTS=${fifo_test_slots})
	target_link_libraries(test-fifo-ring Threads::Threads)

	add_test(NAME fifo-ring COMMAND test-fifo-ring)
	# The threaded case publishes a million records through a seven-slot
	# ring, the queue case sixty thousand more: half a second here and in
	# CI. The limit only catches a run whose threads do not finish.
	set_tests_properties(fifo-ring PROPERTIES TIMEOUT 30)

	# The same source again under a thread sanitizer, which is what pins
	# the ring's release and acquire ordering: a plain run catches one or
	# two of the four weakened, depending on the optimisation level,
	# because the processor or the compiler was doing it anyway, while the
	# sanitizer reports any of the four.
	#
	# Only where the compiler has a sanitizer that works. The probe is
	# built and run, not only compiled: GCC needs libtsan installed to
	# link at all, and a runtime that links can still refuse to start, as
	# one does on a recent kernel with 32-bit address-space randomisation.
	# The test is simply absent where the probe fails.
	include(CheckCSourceRuns)
	set(CMAKE_REQUIRED_FLAGS -fsanitize=thread)
	check_c_source_runs("int main(void) { return 0; }"
		HAVE_THREAD_SANITIZER)
	unset(CMAKE_REQUIRED_FLAGS)

	if(HAVE_THREAD_SANITIZER)
		add_executable(test-fifo-ring-tsan
			test_fifo_ring.c
			${libubertooth_SOURCE_DIR}/src/ubertooth_fifo.c)
		target_include_directories(test-fifo-ring-tsan PRIVATE
			${libubertooth_SOURCE_DIR}/src)
		target_compile_definitions(test-fifo-ring-tsan PRIVATE
			UBERTOOTH_FIFO_SLOTS_FOR_TESTS=${fifo_test_slots})
		target_compile_options(test-fifo-ring-tsan PRIVATE
			-fsanitize=thread -g)
		# LINK_FLAGS rather than target_link_options(), which wants
		# CMake 3.13 and would stop a configure at this project's
		# declared floor of 3.5 with an unknown command.
		set_target_properties(test-fifo-ring-tsan PROPERTIES
			LINK_FLAGS -fsanitize=thread)
		target_link_libraries(test-fifo-ring-tsan Threads::Threads)

		add_test(NAME fifo-ring-tsan COMMAND test-fifo-ring-tsan)
		# A sanitized run of the same records takes a few seconds
		# rather than half of one. halt_on_error stops at the first
		# race, which aborts the process, so a race is a failure.
		#
		# log_path=stdout matters: the threaded case holds standard
		# error on a temporary file of its own, to count the ring's
		# overflow warnings, and a report written there would die with
		# the file. Standard output is untouched, so ctest
		# --output-on-failure prints the report.
		#
		# A sanitizer that will not start is a skip, not a failure.
		# On a kernel that randomizes addresses over more bits than
		# the sanitizer's own mapping allows (6.6 and later with
		# vm.mmap_rnd_bits=32) it gives up before main() with
		# "unexpected memory mapping" -- on four runs in five on one
		# machine here, which is also why the configure probe above
		# cannot predict it: the same program starts on some runs and
		# not others. That says nothing about the ring either way.
		set_tests_properties(fifo-ring-tsan PROPERTIES
			TIMEOUT 120
			ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:log_path=stdout"
			SKIP_REGULAR_EXPRESSION
			"ThreadSanitizer: unexpected memory mapping")
	endif()

	# The arithmetic in the Basic Rate receive callbacks: the symbol count
	# the decoder is told it has, the CLKN a timestamp comes to, the signal
	# level a channel's first packets report, and the clock trim a piconet
	# follow sends.
	#
	# The test includes ubertooth_callback.c itself, so the real cb_rx() and
	# cb_scan() run and their static helpers can be called directly. Nothing
	# else of the library is linked: the test defines the fifo calls, the
	# symbol unpacking, the control requests and the libbtbb entry points it
	# wants to watch, which is what lets it see what each of them was
	# given. libbtbb is
	# linked for the packet and piconet the callbacks really allocate.
	add_executable(test-bredr-callback test_bredr_callback.c)
	target_include_directories(test-bredr-callback PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-bredr-callback ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME bredr-callback COMMAND test-bredr-callback)
	# Arithmetic and a handful of callback calls against a faked ring
	# buffer: milliseconds.
	set_tests_properties(bredr-callback PROPERTIES TIMEOUT 5)

	# What the low-energy callback hands libbtbb's decoder: the captured
	# bytes and how many of them there are, never more.
	#
	# The test includes ubertooth_callback.c itself, so the real cb_btle()
	# runs. Nothing touches a device or does a real decode: the test defines
	# its own stand-in for the decoder, and the fifo calls are faked in the
	# test (ubertooth_fifo.c is not linked), as in the tests above. libbtbb
	# is still linked, for the packet allocation and the symbols the other
	# callbacks reference.
	add_executable(test-le-decode-buffer
		test_le_decode_buffer.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c)
	target_include_directories(test-le-decode-buffer PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-le-decode-buffer ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME le-decode-buffer COMMAND test-le-decode-buffer)
	# A few callback calls against a faked ring buffer: milliseconds.
	set_tests_properties(le-decode-buffer PROPERTIES TIMEOUT 5)

	# What the low-energy callback does with what the PCAPNG writer tells
	# it: one warning when the connection parameters a capture carried
	# could not be recorded, one when the write failed, and each said once
	# for the whole capture.
	#
	# Built the same way as the test above: ubertooth_callback.c is
	# compiled into the test, and the writer and the decoder are the
	# test's own, so no file is written and nothing is decoded.
	add_executable(test-le-capture-reports
		test_le_capture_reports.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c)
	target_include_directories(test-le-capture-reports PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-le-capture-reports ${LIBUSB_LIBRARIES}
		${LIBBTBB_LIBRARIES} Threads::Threads)
	add_test(NAME le-capture-reports COMMAND test-le-capture-reports)
	# A handful of callback calls against a faked ring buffer: milliseconds.
	set_tests_properties(le-capture-reports PROPERTIES TIMEOUT 5)

	# The channel a record claims, where the two AFH callbacks index a
	# 79-entry array of per-channel counters with it. Same approach: the
	# real callbacks run, with the ring buffer, the board's requests and
	# libbtbb's access-code search faked in the test.
	add_executable(test-afh-channel-bounds
		test_afh_channel_bounds.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
	target_include_directories(test-afh-channel-bounds PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIRS} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-afh-channel-bounds ${LIBUSB_LIBRARIES}
		${LIBBTBB_LIBRARIES} Threads::Threads)
	add_test(NAME afh-channel-bounds COMMAND test-afh-channel-bounds)
	# Eight callback calls against a faked ring buffer: milliseconds.
	set_tests_properties(afh-channel-bounds PROPERTIES TIMEOUT 5)

	# What the library does when a capture's USB stream fails part-way: the
	# transfer callback driven straight from the test, and whole streams
	# driven through the faked poll loop. The libusb and fifo calls are the
	# test's own (ubertooth_fifo.c is not linked, for the same reason as
	# above), so no board or ring buffer is involved.
	add_executable(test-stream-failure
		test_stream_failure.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-stream-failure PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	# ${CMAKE_DL_LIBS} for dlsym(): the test defines pthread_create() so a
	# case can refuse a capture's collecting thread, and looks the real one
	# up past its own definition for the cases that need a thread.
	target_link_libraries(test-stream-failure ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads ${CMAKE_DL_LIBS})
	add_test(NAME stream-failure COMMAND test-stream-failure)
	# Every event is delivered by the fake as soon as it is asked for, so a
	# run takes milliseconds. A failure that no longer stops the receive loop
	# would hang here; fail it instead.
	set_tests_properties(stream-failure PROPERTIES TIMEOUT 10)

	# The keep-alive a quiet capture gets: what the receive callback
	# publishes for a transfer shorter than a whole packet, which of those
	# it reports as a fault, and what a dump writes for a keep-alive that
	# reaches the consumer anyway. The fifo fake here is a small ring, so a
	# case can see which slots were published and what was in them.
	add_executable(test-keep-alive
		test_keep_alive.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-keep-alive PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-keep-alive ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME keep-alive COMMAND test-keep-alive)
	# A handful of callback calls and one short capture: milliseconds. A
	# capture that no longer stops would hang here; fail it instead.
	set_tests_properties(keep-alive PROPERTIES TIMEOUT 10)

	# What a capture record says about the noise power of its packet, read
	# back out of written pcap and pcapng files for both the low-energy and
	# the Basic Rate path. The real callbacks run and libbtbb really
	# decodes and writes, because libbtbb is where the flags are assembled;
	# the fifo calls are the test's own, so no board or 72 MB ring buffer is
	# involved.
	add_executable(test-capture-noise-flag
		test_capture_noise_flag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-capture-noise-flag PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-capture-noise-flag ${LIBUSB_LIBRARIES}
		${LIBBTBB_LIBRARIES} Threads::Threads)
	add_test(NAME capture-noise-flag COMMAND test-capture-noise-flag)
	# Two callback calls and four small files: milliseconds.
	set_tests_properties(capture-noise-flag PROPERTIES TIMEOUT 10)

	# What the low-energy callback prints, on which stream, and with what
	# signal level beside each packet. The real cb_btle() and the real
	# decoder run over one crafted packet, so the complaint the cases look
	# for is libbtbb's own; only the fifo calls are faked
	# (ubertooth_fifo.c is not linked, for the same reason as above).
	add_executable(test-btle-dump-streams
		test_btle_dump_streams.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-btle-dump-streams PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-btle-dump-streams ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME btle-dump-streams COMMAND test-btle-dump-streams)
	# A handful of callback calls over one packet each, and two short
	# captures: milliseconds.
	set_tests_properties(btle-dump-streams PROPERTIES TIMEOUT 5)

	# How a signal stops a capture: the real library handlers, and the waits
	# every receive loop makes. No device and no transfer; the fifo calls are
	# the test's own (ubertooth_fifo.c is not linked, for the same reason as
	# above).
	add_executable(test-stop-on-signal
		test_stop_on_signal.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_le_frag.c)
	target_include_directories(test-stop-on-signal PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-stop-on-signal ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES}
		Threads::Threads)
	add_test(NAME stop-on-signal COMMAND test-stop-on-signal)
	# Raising a signal and reading a flag: milliseconds. A wait that no
	# longer notices the stop would hang here; fail it instead.
	set_tests_properties(stop-on-signal PROPERTIES TIMEOUT 10)
endif()

# The same clock wrap through a replay, so the times a capture file carries are
# checked end to end with the real ubertooth-rx.
add_test(
	NAME rx-clock-wrap
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_rx_clock_wrap.py
		$<TARGET_FILE:ubertooth-rx>)
# About 3.4 s on an idle machine and 7 s on one busy with parallel builds. The
# old thirty seconds matched the test's own per-replay deadline, so a loaded
# machine was cut off here with nothing said and a hang was never reported by
# the test. Two minutes leaves that deadline room to fire first and say so.
set_tests_properties(rx-clock-wrap PROPERTIES TIMEOUT 120)

# host/doc/man: a build must leave the committed man pages alone, and the
# refresh-manpages target must rewrite them, keeping the .TH fields ronn
# leaves empty by default. Configures the real host/doc project against a copy
# of the source tree and builds three targets in it.
add_test(
	NAME manpages
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_manpages.py
		${CMAKE_COMMAND}
		${CMAKE_CURRENT_SOURCE_DIR}/../..)
# One ronn pass over fifteen pages: about 5 s warm and 17 s cold here. Without
# ronn the test skips in well under a second, and reports as skipped rather
# than passed, so a green run cannot hide an absent check.
set_tests_properties(manpages PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# Matches the example output lines in host/doc/*.md against the lines the
# tests here and under host/python pin a tool to print, so a page left behind
# by a reworded message fails. Reads both as source; nothing is built or run.
add_test(
	NAME doc-example-lines
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_doc_example_lines.py
		${CMAKE_CURRENT_SOURCE_DIR}/../..)
set_tests_properties(doc-example-lines PROPERTIES TIMEOUT 60)

# The same approach for ubertooth-scan. Its main() needs the BlueZ headers, so
# like the tool itself this is built only where libbluetooth is installed; the
# find_package here is what makes that visible in this directory. FindBLUETOOTH
# reports through FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBBLUETOOTH), which reads
# LIBBLUETOOTH_FIND_QUIETLY rather than this call's own name, so that is what
# keeps a machine without libbluetooth from repeating the probe's complaint
# after the tools' build summary has already said what is skipped.
set(LIBBLUETOOTH_FIND_QUIETLY TRUE)
find_package(BLUETOOTH)
if(LIBBLUETOOTH_FOUND)
	add_executable(test-scan-main test_scan_main.c ../src/follow_afh.c
		../src/request_report.c)
	target_link_libraries(test-scan-main opt-parse-for-tests
		${LIBBTBB_LIBRARIES} ${LIBBLUETOOTH_LIBRARIES})
	if(libubertooth_SOURCE_DIR)
		target_include_directories(test-scan-main PRIVATE
			${libubertooth_SOURCE_DIR}/src)
		# The real report for a request the board refused, so the cases
		# pin the library's wording rather than a stub's.
		target_sources(test-scan-main PRIVATE
			${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
		target_compile_definitions(test-scan-main PRIVATE TEST_REAL_API_REPORTS)
	else()
		target_include_directories(test-scan-main PRIVATE
			${LIBUBERTOOTH_INCLUDE_DIR})
	endif()
	target_include_directories(test-scan-main PRIVATE ../src
		${LIBBTBB_INCLUDE_DIR}
		${LIBBLUETOOTH_INCLUDE_DIR})
	if(USE_OWN_GNU_GETOPT)
		target_include_directories(test-scan-main BEFORE PRIVATE ../getopt)
		target_link_libraries(test-scan-main libgetopt_static)
	endif()

	add_test(NAME scan-main COMMAND test-scan-main)
	# The BlueZ inquiry is faked and the faked receive ends the capture loop
	# at once; a regression that waits on an adapter or a board should fail
	# instead of hanging the suite.
	set_tests_properties(scan-main PROPERTIES TIMEOUT 10)

	# The same for ubertooth-follow, the other BlueZ tool: the order it sets
	# a board up in. The board's clock must be zeroed before the receive
	# transfer is submitted, and the asynchronous requests must stay after
	# the poll thread that completes them.
	# request_report.c is the real one, so the cases pin the wording the tool
	# prints rather than a stub's.
	add_executable(test-follow-main test_follow_main.c ../src/follow_afh.c
		../src/request_report.c)
	target_link_libraries(test-follow-main opt-parse-for-tests
		${LIBBTBB_LIBRARIES} ${LIBBLUETOOTH_LIBRARIES})
	if(libubertooth_SOURCE_DIR)
		target_include_directories(test-follow-main PRIVATE
			${libubertooth_SOURCE_DIR}/src)
		# The real report for a request the board refused, so the cases
		# pin the library's wording rather than a stub's.
		target_sources(test-follow-main PRIVATE
			${libubertooth_SOURCE_DIR}/src/ubertooth_api_version.c)
		target_compile_definitions(test-follow-main PRIVATE TEST_REAL_API_REPORTS)
	else()
		target_include_directories(test-follow-main PRIVATE
			${LIBUBERTOOTH_INCLUDE_DIR})
	endif()
	target_include_directories(test-follow-main PRIVATE ../src
		${LIBBTBB_INCLUDE_DIR}
		${LIBBLUETOOTH_INCLUDE_DIR})
	if(USE_OWN_GNU_GETOPT)
		target_include_directories(test-follow-main BEFORE PRIVATE ../getopt)
		target_link_libraries(test-follow-main libgetopt_static)
	endif()

	add_test(NAME follow-main COMMAND test-follow-main)
	# Every adapter and board call is faked and the faked receive ends the
	# capture loop after two turns, so a run takes milliseconds; a regression
	# that waits on an adapter or a board should fail instead of hanging.
	set_tests_properties(follow-main PROPERTIES TIMEOUT 10)
endif()

# The same approach for ubertooth-debug, -dump and -tx: their real main()
# against a scripted device, so which exits open one and which give the handle
# back is checked without a board. None of them reaches a capture or transmit
# path; the fakes assert if one ever does.
add_executable(test-debug-main test_debug_main.c
	../src/debug_registers.c ../src/cc2400.c ../src/arglist.c)
add_executable(test-dump-main test_dump_main.c)
add_executable(test-tx-main test_tx_main.c)
foreach(tool debug dump tx)
	target_link_libraries(test-${tool}-main opt-parse-for-tests
		${LIBBTBB_LIBRARIES})
	if(libubertooth_SOURCE_DIR)
		target_include_directories(test-${tool}-main PRIVATE
			${libubertooth_SOURCE_DIR}/src)
	else()
		target_include_directories(test-${tool}-main PRIVATE
			${LIBUBERTOOTH_INCLUDE_DIR})
	endif()
	target_include_directories(test-${tool}-main PRIVATE ../src
		${LIBBTBB_INCLUDE_DIR})
	if(USE_OWN_GNU_GETOPT)
		target_include_directories(test-${tool}-main BEFORE PRIVATE ../getopt)
		target_link_libraries(test-${tool}-main libgetopt_static)
	endif()
	add_test(NAME ${tool}-main COMMAND test-${tool}-main)
	# Nothing here waits on a device; a regression that reaches a real
	# receive or transmit loop should fail instead of hanging the suite.
	set_tests_properties(${tool}-main PROPERTIES TIMEOUT 10)
endforeach()

# FindBTBB.cmake must refuse a libbtbb that does not have the functions these
# tools call, naming them and the pinned commit, instead of letting the build
# reach the compiler and fail there. Runs the real module from a one-line
# project against this build's libbtbb, against a header with two of those
# functions renamed out of it, and against a library that defines none of
# them under optimisation.
# Both are quoted: a path with a space in it, or two libraries in
# LIBBTBB_LIBRARIES, would otherwise arrive as extra arguments and the test
# would report a usage error instead of running.
add_test(
	NAME btbb-check
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_btbb_check.py
		${CMAKE_COMMAND}
		${CMAKE_CURRENT_SOURCE_DIR}/../../..
		"${LIBBTBB_INCLUDE_DIR}"
		"${LIBBTBB_LIBRARIES}")
# Five configures of a project with one C file's worth of compiler detection
# in it, plus the probe compiles and the empty library's own build: 19 s here
# on a loaded machine. The limit has room for a cold CI runner, where
# compiler detection alone takes seconds.
set_tests_properties(btbb-check PROPERTIES TIMEOUT 60)

# FindUSB1.cmake must report every directory a source needs for libusb's
# header, so a target asks libusb for it rather than depending on where
# another library happens to be installed. Runs the real module from a
# one-line project and compiles both spellings of the include with nothing but
# the directories it reports. Then it configures against a libusb directory of
# its own in four shapes, removing what the module remembers each time,
# because a remembered path that a package upgrade has moved must be searched
# for again rather than reported as found.
add_test(
	NAME libusb-include
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_libusb_include.py
		${CMAKE_COMMAND}
		${CMAKE_CURRENT_SOURCE_DIR}/../../..)
# One configure of a two-file project and its build, then six configures of a
# project with no languages in it, which need no compiler and take a fraction
# of a second each: 4 s here altogether, 27 s on a loaded machine, where
# detecting the compiler for the first project is most of it. The same limit
# as btbb-check above, which does five configures.
set_tests_properties(libusb-include PROPERTIES TIMEOUT 60)

# FindBTBB.cmake, FindUBERTOOTH.cmake and FindBLUETOOTH.cmake must check that
# a header directory and a library they remember are still there, because a
# package upgrade moves them: a remembered path taken on trust reported the
# library found and left the build to fail on the missing header. Runs each
# real module from a one-line project against an install of its own in four
# shapes: three that remove what the module remembers, and one that moves the
# install to another prefix, where the module has to find it again.
# LIBBTBB_INCLUDE_DIR and LIBBTBB_LIBRARIES are quoted: the test copies that
# header and library, and a path with a space in it, or two libraries, would
# otherwise arrive as extra arguments and the test would report a usage error
# instead of running.
add_test(
	NAME find-stale-paths
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_find_stale_paths.py
		${CMAKE_COMMAND}
		${CMAKE_CURRENT_SOURCE_DIR}/../../..
		"${LIBBTBB_INCLUDE_DIR}"
		"${LIBBTBB_LIBRARIES}")
# Twenty-one configures, seven of them of a project with C in it for
# FindBTBB's function check and the rest of a project with no languages, which
# needs no compiler: 19 s here. Twice the limit of btbb-check and
# libusb-include above, because this is three times the configures of either,
# and a cold CI runner spends seconds on compiler detection alone.
set_tests_properties(find-stale-paths PROPERTIES TIMEOUT 120)
