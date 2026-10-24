# Minimum Python for the in-tree Python tools (ubertooth-specan-ui, ubtbr).
#
# 3.8 is the oldest version verified in CI (Ubuntu 20.04), and every feature
# of both tools works on it. Which Qt binding release can be installed depends
# on the Python version; that is documented rather than checked here.
set(UBERTOOTH_PYTHON_MIN 3.8)

# ubertooth_python_floor(<found> <version> <require_all> <result_var> <message_var>)
#
# Decide whether to build the Python tools for the interpreter CMake found.
# Sets <result_var> to ENABLED, SKIPPED or FATAL and <message_var> to the text
# to print. The caller does the printing, so this logic can be unit-tested
# with `cmake -P` (see test_python_floor.cmake).
function(ubertooth_python_floor found version require_all result_var message_var)
	if(NOT found)
		set(why "no Python 3 interpreter was found")
	elseif(version VERSION_LESS UBERTOOTH_PYTHON_MIN)
		set(why "Python ${version} is older than ${UBERTOOTH_PYTHON_MIN}")
	else()
		set(${result_var} ENABLED PARENT_SCOPE)
		set(${message_var} "Python tools: ENABLED (Python ${version})" PARENT_SCOPE)
		return()
	endif()

	if(require_all)
		set(${result_var} FATAL PARENT_SCOPE)
		set(${message_var}
			"REQUIRE_ALL is set but ${why}; the Python tools (ubertooth-specan-ui, ubtbr) need Python ${UBERTOOTH_PYTHON_MIN} or newer."
			PARENT_SCOPE)
	else()
		set(${result_var} SKIPPED PARENT_SCOPE)
		set(${message_var}
			"Python tools: SKIPPED (ubertooth-specan-ui, ubtbr): ${why}; they need Python ${UBERTOOTH_PYTHON_MIN} or newer. Set -DREQUIRE_ALL=ON to make this an error."
			PARENT_SCOPE)
	endif()
endfunction()
