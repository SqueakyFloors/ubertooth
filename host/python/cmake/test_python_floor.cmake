# Unit test for ubertooth_python_floor(). Run with:
#   cmake -P host/python/cmake/test_python_floor.cmake
# Prints one PASS/FAIL line per case and fails if any case fails.
include(${CMAKE_CURRENT_LIST_DIR}/UbertoothPythonFloor.cmake)

set(failures 0)

# check(<found> <version> <require_all> <expected_result> <expected_message_regex>)
function(check found version require_all expected regex)
	ubertooth_python_floor("${found}" "${version}" "${require_all}" result text)
	set(name "found=${found} version=${version} REQUIRE_ALL=${require_all}")
	if(result STREQUAL expected AND text MATCHES "${regex}")
		message(STATUS "PASS ${name} -> ${result}")
	else()
		message(STATUS "FAIL ${name}: got ${result} \"${text}\", want ${expected} matching \"${regex}\"")
		math(EXPR n "${failures} + 1")
		set(failures ${n} PARENT_SCOPE)
	endif()
endfunction()

check(TRUE  3.8.2  OFF ENABLED "Python 3.8.2")
check(TRUE  3.8.0  OFF ENABLED "Python 3.8.0")
check(TRUE  3.14.0 OFF ENABLED "Python 3.14.0")
check(TRUE  3.10.1 ON  ENABLED "ENABLED")
check(TRUE  3.7.17 OFF SKIPPED "Python 3.7.17 is older than 3.8.*-DREQUIRE_ALL=ON")
check(TRUE  3.6.9  ON  FATAL   "REQUIRE_ALL is set but Python 3.6.9 is older than 3.8")
check(FALSE ""     OFF SKIPPED "no Python 3 interpreter was found")
check(FALSE ""     ON  FATAL   "REQUIRE_ALL is set but no Python 3 interpreter was found")

if(failures GREATER 0)
	message(FATAL_ERROR "${failures} python floor case(s) failed")
endif()
message(STATUS "all python floor cases passed")
