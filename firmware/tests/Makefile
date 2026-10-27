# Host-side unit tests for Project Ubertooth firmware logic.
#
# These compile pure-C firmware logic files with the host compiler and run
# them natively -- no ARM toolchain, no hardware. Run with:  make -C firmware/tests
# (or `make test` from firmware/). Exits non-zero if any assertion fails, so
# CI can gate on it.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99 -g
# The bootloader is the only C++ in the tree, so its test is the only one
# built with the C++ compiler.
CXX     ?= c++
CXXFLAGS ?= -O2 -Wall -Wextra -g
# Headers under test live in the firmware image dir; tests include by name.
FW_RXTX := ../bluetooth_rxtx
FW_BTBR := ../btbr
FW_COMMON := ../common
FW_LPCUSB := ../common/lpcusb/target
FW_BOOT := ../bootloader
HOST_DFU := ../../host/ubertooth-tools/src
CPPFLAGS += -I. -I$(FW_RXTX) -I../../host/libubertooth/src

TESTS   := test_minunit test_rssi test_bluetooth_hop test_access_code test_identify_blink test_frend test_cc2400_wait test_cc2400_spi_atomic test_mode_dispatch test_mode_state_reset test_le_phy_order test_grmdm_rx_generic test_tx_generic_bounds test_write_registers_bounds test_btbr_hop test_usbhw_wait test_bootloader_flash_bounds test_vendor_request_bounds test_rangetest_radio test_usb_queue test_btbr_usb_send test_compile_info
BINDIR  := bin

.PHONY: all test clean
all: test

# The harness itself: what a failed assertion does. Nothing under test but
# minunit.h, so it links no firmware source.
$(BINDIR)/test_minunit: test_minunit.c minunit.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_minunit.c

# Each test binary links its test driver with the firmware source under test.
$(BINDIR)/test_rssi: test_rssi.c minunit.h $(FW_RXTX)/ubertooth_rssi.c $(FW_RXTX)/ubertooth_rssi.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_rssi.c $(FW_RXTX)/ubertooth_rssi.c

# bluetooth.h includes "ubertooth.h"; stubs/ supplies a hardware-free one.
$(BINDIR)/test_bluetooth_hop: test_bluetooth_hop.c minunit.h stubs/ubertooth.h \
		$(FW_RXTX)/bluetooth.c $(FW_RXTX)/bluetooth.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs $(CPPFLAGS) -o $@ test_bluetooth_hop.c $(FW_RXTX)/bluetooth.c

# The mode dispatch, the request handler and bt_stream_rx() are hardware code,
# so what a mode start resets is modelled (see the test's header comment) and
# the model's shape is scanned in test_mode_dispatch. bluetooth.c is linked in,
# so the AFH map and the hop sequence the model asks about are the firmware's.
$(BINDIR)/test_mode_state_reset: test_mode_state_reset.c minunit.h \
		stubs/ubertooth.h $(FW_RXTX)/bluetooth.c $(FW_RXTX)/bluetooth.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) -Istubs $(CPPFLAGS) -o $@ test_mode_state_reset.c \
		$(FW_RXTX)/bluetooth.c

# bluetooth.c is included into the test translation unit (see the test's
# header comment); stubs/ubertooth.h stands in for the hardware headers.
$(BINDIR)/test_access_code: test_access_code.c minunit.h stubs/ubertooth.h \
		$(FW_RXTX)/bluetooth.c $(FW_RXTX)/bluetooth.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs $(CPPFLAGS) -o $@ test_access_code.c

# The identify blink pattern has no hardware dependency, so it compiles as is.
$(BINDIR)/test_identify_blink: test_identify_blink.c minunit.h \
		$(FW_RXTX)/identify_blink.c $(FW_RXTX)/identify_blink.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_identify_blink.c $(FW_RXTX)/identify_blink.c

# cc2400.h is register definitions plus frend_tx(); it needs no stubs, so it is
# included straight from firmware/common. FW_RXTX_DIR and FW_BTBR_DIR let the
# last test scan both images' sources for hard-coded PA levels (run from this
# directory).
$(BINDIR)/test_frend: test_frend.c minunit.h $(FW_COMMON)/cc2400.h \
		$(FW_RXTX)/bluetooth_rxtx.c $(FW_RXTX)/ego.c \
		$(FW_BTBR)/src/btphy/rf.c \
		$(FW_BTBR)/src/btctl/vendor_request_handler.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(FW_COMMON) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' -o $@ test_frend.c

# cc2400_wait.c reaches the radio only through cc2400_status()/cc2400_strobe();
# stubs-radio/ubertooth.h declares them and the test supplies fake
# implementations.
#
# Its last cases scan the mode sources as text, so FW_RXTX_DIR points at them.
$(BINDIR)/test_cc2400_wait: test_cc2400_wait.c minunit.h source_scan.h \
		stubs-radio/ubertooth.h stubs/ubertooth.h $(FW_RXTX)/cc2400_wait.c \
		$(FW_RXTX)/cc2400_wait.h $(FW_RXTX)/bluetooth_rxtx.c $(FW_RXTX)/ego.c \
		$(FW_RXTX)/le_phy.c $(FW_RXTX)/cc2400_rangetest.c | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-radio $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_cc2400_wait.c $(FW_RXTX)/cc2400_wait.c

# cc2400_spi() is GPIO register writes in a file that also configures the
# clock, so it cannot run here; this test scans firmware/common as text, like
# test_mode_dispatch does. FW_COMMON_DIR points it at the source.
$(BINDIR)/test_cc2400_spi_atomic: test_cc2400_spi_atomic.c minunit.h \
		source_scan.h $(FW_COMMON)/ubertooth.c $(FW_COMMON)/ubertooth.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_COMMON_DIR='"$(FW_COMMON)"' \
		-o $@ test_cc2400_spi_atomic.c

# The mode dispatch lives in main(), which cannot run here, so this test scans
# bluetooth_rxtx.c as text (like test_frend's last case). FW_RXTX_DIR points it
# at the source; it links nothing from the firmware.
$(BINDIR)/test_mode_dispatch: test_mode_dispatch.c minunit.h source_scan.h \
		$(FW_RXTX)/bluetooth_rxtx.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_mode_dispatch.c

# le_phy_main() brings the PHY up and tears it down around le_phy_active, and
# the order of those writes is what keeps a stray interrupt harmless. It is
# hardware code, so this test scans le_phy.c as text too.
$(BINDIR)/test_le_phy_order: test_le_phy_order.c minunit.h source_scan.h \
		$(FW_RXTX)/le_phy.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_le_phy_order.c

# grmdm_packet_mode() is a bit test in cc2400.h, so it runs here; the
# rx_generic() that uses it is hardware code, so the same test scans
# bluetooth_rxtx.c for the shape.
$(BINDIR)/test_grmdm_rx_generic: test_grmdm_rx_generic.c minunit.h \
		source_scan.h $(FW_COMMON)/cc2400.h $(FW_RXTX)/bluetooth_rxtx.c \
		| $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(FW_COMMON) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_grmdm_rx_generic.c

# The generic transmit request's handler and tx_generic() are hardware code, so
# the bounds on a transmit packet are exercised through fakes and the firmware's
# own shape is scanned, as test_frend does. generic_tx_packet and DMA_SIZE come
# from the shared interface header, already on the include path.
$(BINDIR)/test_tx_generic_bounds: test_tx_generic_bounds.c minunit.h \
		source_scan.h $(FW_RXTX)/bluetooth_rxtx.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_tx_generic_bounds.c

# The multi-register write's handler is hardware code too -- it writes radio
# registers -- so the bound on its register count is exercised through a fake
# of the case, and the firmware's own shape is scanned across the three files
# the bound is spread over.
$(BINDIR)/test_write_registers_bounds: test_write_registers_bounds.c minunit.h \
		source_scan.h $(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_RXTX)/ubertooth_usb.c $(FW_RXTX)/ubertooth_usb.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_write_registers_bounds.c

# hop.c in the btbr image reaches the hardware only through ubtbr/debug.h,
# which pulls in the console and USB headers and an ARM inline-asm helper;
# stubs-btbr/ supplies a hardware-free stand-in. -Istubs-btbr comes first so
# it wins. perm5.c is the pre-computed PERM5 table and nothing else, so it
# links as is; the test supplies the btphy state.
$(BINDIR)/test_btbr_hop: test_btbr_hop.c minunit.h stubs-btbr/ubtbr/debug.h \
		$(FW_BTBR)/src/btphy/hop.c $(FW_BTBR)/src/codec/perm5.c \
		$(FW_BTBR)/include/ubtbr/hop.h $(FW_BTBR)/include/ubtbr/btphy.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-btbr -I$(FW_BTBR)/include $(CPPFLAGS) \
		-o $@ test_btbr_hop.c $(FW_BTBR)/src/btphy/hop.c \
		$(FW_BTBR)/src/codec/perm5.c

# The USB hardware layer reaches the controller only through the register
# macros in lpc17.h, so stubs-usb/ supplies a fake one and this links the real
# usbhw_lpc.c against it. -DLPC17xx picks that branch of the file; -Istubs-usb
# comes first so its lpc17.h wins over firmware/common's.
$(BINDIR)/test_usbhw_wait: test_usbhw_wait.c minunit.h stubs-usb/lpc17.h \
		$(FW_LPCUSB)/usbhw_lpc.c $(FW_LPCUSB)/usbhw_lpc.h \
		$(FW_LPCUSB)/usbcontrol.c $(FW_COMMON)/usb_irq.h \
		$(FW_RXTX)/ubertooth_usb.h \
		$(FW_BTBR)/include/ubtbr/ubertooth_usb.h | $(BINDIR)
# Two kinds of noise come from compiling this vendored code on a 64-bit host
# with the debug macros off: the DMA descriptor helpers cast pointers to U32,
# and usbcontrol.c's stall path builds a dump the disabled DBG() would have
# printed. Neither is what this tests.
	$(CC) $(CFLAGS) -Istubs-usb -I$(FW_LPCUSB) $(CPPFLAGS) -DLPC17xx \
		-DFW_LPCUSB_DIR='"$(FW_LPCUSB)"' -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' -DFW_COMMON_DIR='"$(FW_COMMON)"' \
		-Wno-pointer-to-int-cast -Wno-unused-but-set-variable \
		-Wno-unused-parameter -o $@ test_usbhw_wait.c \
		$(FW_LPCUSB)/usbhw_lpc.c $(FW_LPCUSB)/usbcontrol.c
# The packet queue is pure logic, so ubertooth_usb.c is included into the test
# (see its header comment) and the test supplies the bulk IN endpoint.
# stubs-usbq/ stands in for ubertooth.h and lpcusb's usbapi.h and comes first
# so it wins; usbhw_lpc.h is register #defines and prototypes with no hardware
# dependency, so the real one is used.
#
# Its last cases scan the mode loops for which USB service each one calls, so
# FW_RXTX_DIR points at those sources too.
$(BINDIR)/test_usb_queue: test_usb_queue.c minunit.h source_scan.h \
		stubs-usbq/usbapi.h \
		stubs-usbq/ubertooth.h stubs/ubertooth.h \
		$(FW_RXTX)/ubertooth_usb.c $(FW_RXTX)/ubertooth_usb.h \
		$(FW_RXTX)/bluetooth_rxtx.c $(FW_RXTX)/le_phy.c $(FW_RXTX)/ego.c \
		$(FW_COMMON)/usb_irq.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-usbq $(CPPFLAGS) -I$(FW_LPCUSB) \
		-I$(FW_COMMON) -DFW_HOST_TEST -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_usb_queue.c

# The part table in flash.h and Flash::valid_address compile here as they are,
# with the In-Application Programming read behind them faked by the test, so
# the bound is the real one. dfu.cpp cannot be compiled here -- it casts a
# pointer to uint32_t -- so request_upload()'s shape is faked and the test
# scans dfu.cpp for it, as test_tx_generic_bounds does. It also puts the
# host's copy of the part table against the bootloader's, so the paths to both
# sources are passed in.
$(BINDIR)/test_bootloader_flash_bounds: test_bootloader_flash_bounds.cpp \
		minunit.h source_scan.h $(FW_BOOT)/flash.h $(FW_BOOT)/iap.h \
		$(FW_BOOT)/dfu.cpp $(HOST_DFU)/dfu_recovery.c | $(BINDIR)
	$(CXX) $(CXXFLAGS) -I$(FW_BOOT) $(CPPFLAGS) \
		-DFW_BOOTLOADER_DIR='"$(FW_BOOT)"' -DHOST_DFU_DIR='"$(HOST_DFU)"' \
		-o $@ test_bootloader_flash_bounds.cpp
# The request handler and the USB stack are both hardware code, so the bounds
# on a request's data are exercised through fakes and the firmware's own shape
# is scanned: the handler and the access code loop, both images' USB shims, and
# the direction bit in the vendored lpcusb header the shims read.
$(BINDIR)/test_vendor_request_bounds: test_vendor_request_bounds.c minunit.h \
		source_scan.h $(FW_COMMON)/cc2400.h $(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_RXTX)/ego.c \
		$(FW_RXTX)/ubertooth_usb.c $(FW_BTBR)/src/drivers/ubertooth_usb.c \
		$(FW_BTBR)/src/btctl/vendor_request_handler.c \
		$(FW_COMMON)/lpcusb/target/usbstruct.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(FW_COMMON) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' -DFW_COMMON_DIR='"$(FW_COMMON)"' \
		-o $@ test_vendor_request_bounds.c

# The btbr image's USB shim reaches the controller only through the vendored
# stack's USBHw* calls and the NVIC enable registers, so this links the real
# shim against fakes for both: stubs-btbr-usb/ supplies a hardware-free
# ubertooth.h with a fake interrupt controller and a PRIMASK-free system.h,
# stubs-btbr/ the diagnostics stand-in, and the test implements the
# controller. queue.c is the message queue the shim sends from. firmware/common
# is last on the include path, for the shared usb_irq.h; "ubertooth.h" still
# resolves to the stub, which comes first.
$(BINDIR)/test_btbr_usb_send: test_btbr_usb_send.c minunit.h \
		stubs-btbr-usb/ubertooth.h stubs-btbr-usb/ubtbr/system.h \
		stubs-btbr/ubtbr/debug.h \
		$(FW_BTBR)/src/drivers/ubertooth_usb.c \
		$(FW_BTBR)/src/util/queue.c \
		$(FW_BTBR)/include/ubtbr/ubertooth_usb.h \
		$(FW_COMMON)/usb_irq.h | $(BINDIR)
# usb_bulk_out_handler() takes the arguments the stack's endpoint callback
# passes and uses neither, which is not what this tests.
	$(CC) $(CFLAGS) -Istubs-btbr-usb -Istubs-btbr -I$(FW_BTBR)/include \
		-I$(FW_LPCUSB) -I$(FW_COMMON) $(CPPFLAGS) -DFW_HOST_TEST \
		-Wno-unused-parameter \
		-o $@ test_btbr_usb_send.c \
		$(FW_BTBR)/src/drivers/ubertooth_usb.c $(FW_BTBR)/src/util/queue.c

# The build string in both images is what ubertooth-dfu -r reads the expected
# revision out of, so this test links the host's real extractor and scans both
# images' definitions, the way test_bootloader_flash_bounds puts the
# bootloader's part table against the host's copy. compile_info itself cannot
# be compiled here: it is built from the generated version.h, which only exists
# once an image has been built.
$(BINDIR)/test_compile_info: test_compile_info.c minunit.h source_scan.h \
		$(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_BTBR)/src/btctl/vendor_request_handler.c \
		$(HOST_DFU)/dfu_recovery.c $(HOST_DFU)/dfu_recovery.h \
		$(HOST_DFU)/dfu_transition.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(HOST_DFU) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' \
		-o $@ test_compile_info.c $(HOST_DFU)/dfu_recovery.c \
		$(HOST_DFU)/dfu_transition.c

# The range test and the repeater are hardware loops, so this scans
# cc2400_rangetest.c as text for the radio writes they need and for the shape
# of the repeater's turnaround delay, the way test_mode_dispatch.c scans the
# dispatch. It links nothing from the firmware.
$(BINDIR)/test_rangetest_radio: test_rangetest_radio.c minunit.h source_scan.h \
		$(FW_RXTX)/cc2400_rangetest.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_rangetest_radio.c

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
