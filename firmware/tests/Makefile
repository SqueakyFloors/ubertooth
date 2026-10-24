# Host-side unit tests for Project Ubertooth firmware logic.
#
# These compile pure-C firmware logic files with the host compiler and run
# them natively -- no ARM toolchain, no hardware. Run with:  make -C firmware/tests
# (or `make test` from firmware/). Exits non-zero if any assertion fails, so
# CI can gate on it.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99 -g
# Headers under test live in the firmware image dir; tests include by name.
FW_RXTX := ../bluetooth_rxtx
CPPFLAGS += -I. -I$(FW_RXTX)

TESTS   := test_rssi test_access_code
BINDIR  := bin

.PHONY: all test clean
all: test

# Each test binary links its test driver with the firmware source under test.
$(BINDIR)/test_rssi: test_rssi.c minunit.h $(FW_RXTX)/ubertooth_rssi.c $(FW_RXTX)/ubertooth_rssi.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_rssi.c $(FW_RXTX)/ubertooth_rssi.c

# bluetooth.c is included into the test translation unit (see the test's
# header comment); ubertooth.h here stands in for the hardware headers.
$(BINDIR)/test_access_code: test_access_code.c minunit.h ubertooth.h \
		$(FW_RXTX)/bluetooth.c $(FW_RXTX)/bluetooth.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_access_code.c

$(BINDIR):
	mkdir -p $(BINDIR)

# Build and run every test; stop on the first failure.
test: $(addprefix $(BINDIR)/,$(TESTS))
	@fail=0; for t in $(addprefix $(BINDIR)/,$(TESTS)); do \
		echo "== running $$t =="; \
		$$t || fail=1; \
		echo; \
	done; \
	if [ $$fail -ne 0 ]; then echo "UNIT TESTS FAILED"; exit 1; fi; \
	echo "ALL UNIT TESTS PASSED"

clean:
	rm -rf $(BINDIR)
