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
