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

add_test(
	NAME ducky-options
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_ducky_options.py
		$<TARGET_FILE:ubertooth-ducky>)

add_executable(test-specan-options
	test_specan_options.c
	../src/specan_options.c)

target_include_directories(test-specan-options PRIVATE ../src)

add_test(NAME specan-options COMMAND test-specan-options)

add_test(
	NAME specan-cli
	COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_specan_cli.py
		$<TARGET_FILE:ubertooth-specan>)

add_executable(test-btle-options
	test_btle_options.c
	../src/btle_options.c)

target_include_directories(test-btle-options PRIVATE ../src)

add_test(NAME btle-options COMMAND test-btle-options)

add_executable(test-ego-options
	test_ego_options.c
	../src/ego_options.c)

target_include_directories(test-ego-options PRIVATE ../src)

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

# Runs ubertooth-util's real main() against a scripted fake device. The source
# is included directly (main renamed) so it needs the library headers, and the
# bundled getopt on platforms that use it.
add_executable(test-util-main
	test_util_main.c
	../src/util_report.c)

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

	# The same approach for the firmware revision and compile info commands.
	add_executable(test-control-text
		test_control_text.c
		${libubertooth_SOURCE_DIR}/src/ubertooth_control.c)
	target_include_directories(test-control-text PRIVATE
		${libubertooth_SOURCE_DIR}/src ${LIBUSB_INCLUDE_DIR} ${LIBBTBB_INCLUDE_DIR})
	target_link_libraries(test-control-text ${LIBUSB_LIBRARIES})
	add_test(NAME control-text COMMAND test-control-text)
endif()
