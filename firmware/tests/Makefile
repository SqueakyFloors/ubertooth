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
FW_COMMON := ../common
CPPFLAGS += -I. -I$(FW_RXTX)

TESTS   := test_rssi test_bluetooth_hop test_access_code test_identify_blink test_frend test_cc2400_wait
BINDIR  := bin

.PHONY: all test clean
all: test

# Each test binary links its test driver with the firmware source under test.
$(BINDIR)/test_rssi: test_rssi.c minunit.h $(FW_RXTX)/ubertooth_rssi.c $(FW_RXTX)/ubertooth_rssi.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_rssi.c $(FW_RXTX)/ubertooth_rssi.c

# bluetooth.h includes "ubertooth.h"; stubs/ supplies a hardware-free one.
$(BINDIR)/test_bluetooth_hop: test_bluetooth_hop.c minunit.h stubs/ubertooth.h \
		$(FW_RXTX)/bluetooth.c $(FW_RXTX)/bluetooth.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs $(CPPFLAGS) -o $@ test_bluetooth_hop.c $(FW_RXTX)/bluetooth.c

# bluetooth.c is included into the test translation unit (see the test's
# header comment); ubertooth.h here stands in for the hardware headers.
$(BINDIR)/test_access_code: test_access_code.c minunit.h ubertooth.h \
		$(FW_RXTX)/bluetooth.c $(FW_RXTX)/bluetooth.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_access_code.c

# The identify blink pattern has no hardware dependency, so it compiles as is.
$(BINDIR)/test_identify_blink: test_identify_blink.c minunit.h \
		$(FW_RXTX)/identify_blink.c $(FW_RXTX)/identify_blink.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_identify_blink.c $(FW_RXTX)/identify_blink.c

# cc2400.h is register definitions plus frend_tx(); it needs no stubs, so it is
# included straight from firmware/common. FW_RXTX_DIR lets the last test scan
# the transmit sources for hard-coded PA levels (run from this directory).
$(BINDIR)/test_frend: test_frend.c minunit.h $(FW_COMMON)/cc2400.h \
		$(FW_RXTX)/bluetooth_rxtx.c $(FW_RXTX)/ego.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(FW_COMMON) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_frend.c

# cc2400_wait.c reaches the radio only through cc2400_status()/cc2400_strobe();
# stubs-radio/ubertooth.h declares them and the test supplies fake
# implementations. -Istubs-radio comes before -I. so it wins over ubertooth.h
# in this directory.
$(BINDIR)/test_cc2400_wait: test_cc2400_wait.c minunit.h stubs-radio/ubertooth.h \
		stubs/ubertooth.h $(FW_RXTX)/cc2400_wait.c $(FW_RXTX)/cc2400_wait.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-radio $(CPPFLAGS) -o $@ test_cc2400_wait.c $(FW_RXTX)/cc2400_wait.c

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
