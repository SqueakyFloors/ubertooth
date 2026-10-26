set(UBTBR_WRAPPER ${CMAKE_CURRENT_BINARY_DIR}/ubertooth-btbr)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/ubertooth-btbr.in
	${UBTBR_WRAPPER}
	@ONLY)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ubtbr
	DESTINATION ${PYTHON_PURELIB_REL}
	FILES_MATCHING PATTERN "*.py"
	PATTERN "__pycache__" EXCLUDE)
install(PROGRAMS ${UBTBR_WRAPPER} DESTINATION ${INSTALL_DEFAULT_BINDIR})

# The test that drives the wrapper is registered beside the other Python
# tests, which needs the path configure_file wrote it to.
set(UBTBR_WRAPPER ${UBTBR_WRAPPER} PARENT_SCOPE)
