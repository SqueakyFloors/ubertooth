add_executable(test-dfu-transition
	test_dfu_transition.c
	../src/dfu_transition.c)

target_include_directories(test-dfu-transition PRIVATE ../src)

add_test(NAME dfu-transition COMMAND test-dfu-transition)

add_executable(test-dfu-recovery
	test_dfu_recovery.c
	../src/dfu_recovery.c
	../src/dfu_transition.c)

target_include_directories(test-dfu-recovery PRIVATE ../src)

add_test(NAME dfu-recovery COMMAND test-dfu-recovery)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ubertooth-dfu checks the image before it opens a device, so the whole
# accept/refuse decision runs against the real binary with no board. Eleven
# short runs, well under a second in total now that the test hides attached
# boards from the tool, so the timeout only catches a tool that waits for a
# device it should never have looked for.
add_test(
	NAME dfu-images
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_dfu_images.py
		$<TARGET_FILE:ubertooth-dfu>)
set_tests_properties(dfu-images PROPERTIES TIMEOUT 15)

# ubertooth-dfu's file handling against the real binary: -s, which files -d
# and -u open or replace, and the files -d and -s refuse to read whole. Needs
# no board, and opens none: the cases that would reach one use a serial no
# board has, with attached boards hidden from the tool. Twenty-one short runs,
# so the timeout only catches a tool that waits for a device.
add_test(
	NAME dfu-files
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_dfu_files.py
		$<TARGET_FILE:ubertooth-dfu>)
set_tests_properties(dfu-files PROPERTIES TIMEOUT 15)

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
endforeach()

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
# board, with attached boards hidden from the tool. Fifteen short runs.
add_test(
	NAME debug-cli
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_debug_cli.py
		$<TARGET_FILE:ubertooth-debug>)
set_tests_properties(debug-cli PROPERTIES TIMEOUT 20)

# The file replay path of ubertooth-rx (-i), on fixtures the test builds
# itself: the recorded times in the packet lines and in a capture written
# during the replay, and the exit status for an unreadable or truncated file.
# No device is involved; the handful of runs take well under a second.
add_test(
	NAME rx-replay
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_rx_replay.py
		$<TARGET_FILE:ubertooth-rx>)
set_tests_properties(rx-replay PROPERTIES TIMEOUT 30)

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
add_executable(test-util-main
	test_util_main.c
	../src/util_report.c
	../src/util_modes.c
	../src/util_reset_wait.c)
target_link_libraries(test-util-main opt-parse-for-tests)

# The library headers of this source tree must win over any installed copy.
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-util-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
else()
	target_include_directories(test-util-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-util-main PRIVATE ../src
	${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-util-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-util-main libgetopt_static)
endif()

add_test(NAME util-main COMMAND test-util-main)
# A regression that lets a mode such as -I (identify, blinks until
# interrupted) run inside main() would hang; fail it instead. A normal run
# takes well under a second (0.02-0.5 s locally and in CI).
set_tests_properties(util-main PROPERTIES TIMEOUT 5)

# The same approach for ubertooth-afh: its real main() against a scripted
# detection phase, so what it reports and exits with is checked without a
# board or a piconet. libbtbb is the real one (no device is touched).
add_executable(test-afh-main test_afh_main.c)
target_link_libraries(test-afh-main opt-parse-for-tests ${LIBBTBB_LIBRARIES})
if(libubertooth_SOURCE_DIR)
	target_include_directories(test-afh-main PRIVATE ${libubertooth_SOURCE_DIR}/src)
else()
	target_include_directories(test-afh-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-afh-main PRIVATE ../src
	${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-afh-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-afh-main libgetopt_static)
endif()

add_test(NAME afh-main COMMAND test-afh-main)
# Nothing here waits on a device; a regression that reaches a real receive
# loop should fail instead of hanging the suite.
set_tests_properties(afh-main PROPERTIES TIMEOUT 10)

# The same approach for ubertooth-btle: its real main() against a scripted
# device, so which commands the -v options run, in which order, is checked
# without a board. libbtbb is the real one (the capture options are not used,
# and no device is touched).
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
	${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-btle-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-btle-main libgetopt_static)
endif()

add_test(NAME btle-main COMMAND test-btle-main)
# The fake poll ends the capture loop at once; a regression that waits on a
# real device should fail instead of hanging the suite.
set_tests_properties(btle-main PROPERTIES TIMEOUT 10)

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
else()
	target_include_directories(test-dfu-main PRIVATE ${LIBUBERTOOTH_INCLUDE_DIR})
endif()
target_include_directories(test-dfu-main PRIVATE ../src
	${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
target_link_libraries(test-dfu-main ${LIBUSB_LIBRARIES})
if(USE_OWN_GNU_GETOPT)
	target_include_directories(test-dfu-main BEFORE PRIVATE ../getopt)
	target_link_libraries(test-dfu-main libgetopt_static)
endif()

add_test(NAME dfu-main COMMAND test-dfu-main)
# The unbounded bootloader state loop this test covers would hang here; fail
# it instead. A normal run takes about 2.6 seconds locally and 4 on the
# slowest CI runner: the transfers are faked, but the case where the board
# comes back on its old firmware waits out cleanup's real hunt for the DFU
# device.
set_tests_properties(dfu-main PROPERTIES TIMEOUT 10)

# Runs the real libubertooth part-ID commands against a fake libusb control
# transfer (defined in the test, so it wins over libusb's). libbtbb is faked;
# libusb is linked for the other symbols. Needs the library sources, so only
# when built from the whole host tree.
if(libubertooth_SOURCE_DIR)
	add_executable(test-control-partnum
		test_control_partnum.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-control-partnum PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-control-partnum ${LIBUSB_LIBRARIES})
	add_test(NAME control-partnum COMMAND test-control-partnum)

	# The same approach for the fixed-size read commands (short replies).
	add_executable(test-control-reads
		test_control_reads.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-control-reads PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-control-reads ${LIBUSB_LIBRARIES})
	add_test(NAME control-reads COMMAND test-control-reads)

	# The same approach for the asynchronous commands (follow mode), with the
	# three libusb transfer calls faked in the test: what the library hands
	# libusb, and what it frees, without a board.
	add_executable(test-control-async
		test_control_async.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-control-async PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
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
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-init-failure PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-init-failure ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES})
	add_test(NAME init-failure COMMAND test-init-failure)
	# Nothing here waits on a device; a regression that reaches USB should
	# fail instead of hanging the suite.
	set_tests_properties(init-failure PROPERTIES TIMEOUT 10)

	# The same approach for the firmware revision and compile info commands.
	add_executable(test-control-text
		test_control_text.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-control-text PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-control-text ${LIBUSB_LIBRARIES})
	add_test(NAME control-text COMMAND test-control-text)

	# Runs the real libubertooth device selection against a faked USB device
	# list (the libusb device calls are defined in the test), so the
	# multi-device -U cases are covered without attaching several boards.
	add_executable(test-device-select
		test_device_select.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_fifo.c)
	target_include_directories(test-device-select PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-device-select ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES})
	add_test(NAME device-select COMMAND test-device-select)
	# Nothing here waits on hardware; a regression that blocks on a real USB
	# call should fail instead of hanging the suite.
	set_tests_properties(device-select PROPERTIES TIMEOUT 10)

	# The control requests the AFH paths send, in order, against the same
	# faked libusb: AFH detection has to run in the board's AFH mode, so
	# UBERTOOTH_RX_SYMBOLS must never be sent on top of it. The fifo calls
	# are faked in the test (ubertooth_fifo.c is not linked), which is also
	# what stops the receive loops.
	add_executable(test-afh-requests
		test_afh_requests.c
		${libubertooth_SOURCE_DIR}/src/ubertooth.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_callback.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-afh-requests PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-afh-requests ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES})
	find_package(Threads REQUIRED)
	target_link_libraries(test-afh-requests Threads::Threads)
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
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_fifo.c)
	target_include_directories(test-clk100ns-wrap PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-clk100ns-wrap ${LIBUSB_LIBRARIES} ${LIBBTBB_LIBRARIES})
	# One case reads ubertooth-btle's source, which starts the mode whose
	# packets come from the other clock (like test_mode_dispatch does).
	target_compile_definitions(test-clk100ns-wrap PRIVATE
		BTLE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../src/ubertooth-btle.c")
	add_test(NAME clk100ns-wrap COMMAND test-clk100ns-wrap)
	# Arithmetic only: a run takes milliseconds.
	set_tests_properties(clk100ns-wrap PROPERTIES TIMEOUT 5)
endif()

# The same clock wrap through a replay, so the times a capture file carries are
# checked end to end with the real ubertooth-rx.
add_test(
	NAME rx-clock-wrap
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_rx_clock_wrap.py
		$<TARGET_FILE:ubertooth-rx>)
set_tests_properties(rx-clock-wrap PROPERTIES TIMEOUT 30)
