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
FW_BADGE := ../tc13badge
FW_DIR := ..
HOST_DFU := ../../host/ubertooth-tools/src
HOST_LIB := ../../host/libubertooth/src
CPPFLAGS += -I. -I$(FW_RXTX) -I../../host/libubertooth/src

TESTS := test_minunit test_rssi test_bluetooth_hop test_identify_blink test_frend test_cc2400_wait test_cc2400_spi_atomic test_mode_dispatch test_mode_state_reset test_promisc_hop_interval test_le_phy_order test_le_frag_shape test_le_frag_enqueue test_le_filter_bounds test_le_phy_logic test_le_rx_byte_wait test_le_state_reset test_grmdm_rx_generic test_tx_generic_bounds test_ego_rx_bounds test_write_registers_bounds test_btbr_hop test_btbr_rx_deadline test_btbr_arithmetic test_btbr_fault_blink test_usbhw_wait test_bootloader_flash_bounds test_bootloader_app_image test_vendor_request_bounds test_vendor_request_values test_rangetest_radio test_usb_queue test_btbr_usb_send test_compile_info test_reset_order test_firmware_object_paths test_fw_info test_watchdog test_latent_arithmetic test_btbr_codec_fec23
BINDIR  := bin
# Where the ToorCon badge's generated register header is written; see the
# $(BINDIR)/tc13badge_syntax.stamp rule at the end of this file.
TC13_GEN := $(BINDIR)/tc13badge

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
$(BINDIR)/test_bluetooth_hop: test_bluetooth_hop.c minunit.h spec_hop_vectors.h \
		stubs/ubertooth.h \
		$(FW_RXTX)/bluetooth.c $(FW_RXTX)/bluetooth.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs $(CPPFLAGS) -o $@ test_bluetooth_hop.c $(FW_RXTX)/bluetooth.c

# The mode dispatch, the request handler and bt_stream_rx() are hardware code,
# so what a mode start resets is modelled (see the test's header comment) and
# the model's shape is scanned in test_mode_dispatch. bluetooth.c is linked in,
# so the AFH map and the hop sequence the model asks about are the firmware's.
# The low-energy state struct and the reset that clears it are read as text
# from the sources FW_RXTX_DIR points at, for the same reason.
$(BINDIR)/test_mode_state_reset: test_mode_state_reset.c minunit.h \
		source_scan.h stubs/ubertooth.h \
		$(FW_RXTX)/bluetooth.c $(FW_RXTX)/bluetooth.h \
		$(FW_RXTX)/bluetooth_le.h $(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_RXTX)/le_phy.c | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_mode_state_reset.c $(FW_RXTX)/bluetooth.c

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
# implementations. It lives in firmware/common because both firmware images
# wait on the radio, so -I$(FW_COMMON) comes last, after stubs-radio: the stub
# must still win for "ubertooth.h".
#
# Its last cases scan the radio sources of both images, and of the two bench
# images, as text, so FW_RXTX_DIR, FW_BTBR_DIR, FW_COMMON_DIR and FW_DIR point
# at them.
$(BINDIR)/test_cc2400_wait: test_cc2400_wait.c minunit.h source_scan.h \
		stubs-radio/ubertooth.h stubs/ubertooth.h $(FW_COMMON)/cc2400_wait.c \
		$(FW_COMMON)/cc2400_wait.h $(FW_COMMON)/ubertooth.c \
		$(FW_RXTX)/bluetooth_rxtx.c $(FW_RXTX)/ego.c \
		$(FW_RXTX)/le_phy.c $(FW_RXTX)/cc2400_rangetest.c \
		$(FW_BTBR)/src/btphy/rf.c $(FW_BTBR)/src/btphy/scan_task.c \
		$(FW_BTBR)/src/btphy/rx_task.c $(FW_BTBR)/src/btphy/tx_task.c \
		$(FW_BTBR)/include/ubtbr/rf.h \
		$(FW_DIR)/assembly_test/assembly_test.c \
		$(FW_DIR)/cc2400_test/cc2400_test.c \
		$(FW_DIR)/tc13badge/tc13badge.c \
		$(FW_DIR)/tc13badge/toorcon.c | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-radio $(CPPFLAGS) -I$(FW_COMMON) \
		-DFW_RXTX_DIR='"$(FW_RXTX)"' -DFW_BTBR_DIR='"$(FW_BTBR)"' \
		-DFW_COMMON_DIR='"$(FW_COMMON)"' -DFW_DIR='"$(FW_DIR)"' \
		-o $@ test_cc2400_wait.c $(FW_COMMON)/cc2400_wait.c

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
# at the source; it links nothing from the firmware. FW_COMMON_DIR is for the
# one case that reads cc2400_init(), where a radio register write would be
# undone by the reset in clock_start().
$(BINDIR)/test_mode_dispatch: test_mode_dispatch.c minunit.h source_scan.h \
		$(FW_RXTX)/bluetooth_rxtx.c $(FW_COMMON)/ubertooth.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_COMMON_DIR='"$(FW_COMMON)"' -o $@ test_mode_dispatch.c

# The connection-interval estimator reads the clock registers, so this models
# it and takes the rollover period and LE_BASECLK out of the source, the same
# way. FW_RXTX_DIR points it at both files; it links nothing from the firmware.
$(BINDIR)/test_promisc_hop_interval: test_promisc_hop_interval.c minunit.h \
		source_scan.h $(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_RXTX)/ubertooth_clock.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_promisc_hop_interval.c

# le_phy_main() brings the PHY up and tears it down around le_phy_active, and
# the order of those writes is what keeps a stray interrupt harmless. It is
# hardware code, so this test scans le_phy.c as text too.
$(BINDIR)/test_le_phy_order: test_le_phy_order.c minunit.h source_scan.h \
		$(FW_RXTX)/le_phy.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_le_phy_order.c

# Which capture loop hands a whole packet to the fragmenting enqueue, and how
# each one gets the whole packet in the first place. Both loops are hardware
# code, so this scans them as text; the split itself is pure logic and runs
# for real in test_le_frag_enqueue. FW_RXTX_DIR points at both sources.
$(BINDIR)/test_le_frag_shape: test_le_frag_shape.c minunit.h source_scan.h \
		$(FW_RXTX)/le_phy.c $(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_RXTX)/ubertooth_dma.c $(FW_RXTX)/bluetooth_le.h \
		stubs/ubertooth.h ../../host/libubertooth/src/ubertooth_le_frag.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) -Istubs $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_le_frag_shape.c

# The low-energy capture engine, run rather than scanned. stubs-lephy/ puts a
# fake register file under the firmware's own ubertooth.h, so the LED macros,
# the pin numbers and the prototypes are the firmware's: see that header. The
# real packet queue (ubertooth_usb.c) and the engine itself are included into
# the test, because what is under test is static in both; queue.c and
# bluetooth_le.c are pure logic and link as they are. -Istubs-lephy comes
# first so its ubertooth.h and lpc17.h win, then stubs-usbq for the lpcusb
# stand-in the queue needs.
#
# The host's reassembly (ubertooth_le_frag.c) is linked in as well, so a long
# packet makes the whole round trip -- firmware fragments it, host puts it
# back -- the way test_compile_info links the host's revision extractor.
$(BINDIR)/test_le_phy_logic: test_le_phy_logic.c minunit.h \
		stubs-lephy/ubertooth.h stubs-lephy/lpc17.h stubs-usbq/usbapi.h \
		stubs-usbq/watchdog.h \
		$(FW_RXTX)/le_phy.c $(FW_RXTX)/ubertooth_usb.c \
		$(FW_RXTX)/ubertooth_usb.h $(FW_RXTX)/queue.c \
		$(FW_RXTX)/bluetooth_le.c $(FW_RXTX)/bluetooth_le.h \
		$(FW_COMMON)/lpc17.h $(FW_COMMON)/ubertooth.h \
		$(FW_COMMON)/cc2400_wait.h $(FW_COMMON)/usb_irq.h \
		$(FW_RXTX)/ubertooth_clock.h $(FW_RXTX)/ubertooth_dma.h \
		$(HOST_LIB)/ubertooth_le_frag.c $(HOST_LIB)/ubertooth_le_frag.h \
		| $(BINDIR)
# Two kinds of noise come from compiling le_phy.c on a 64-bit host. It
# declares two locals in the connection-request handler that nothing reads,
# and the DMA descriptors it builds hold addresses as 32-bit words, which a
# host pointer does not fit. Neither is what this tests, and neither is this
# change's. UBERTOOTH_ONE is the board the shipped image is built for.
	$(CC) $(CFLAGS) -Istubs-lephy -Istubs-usbq $(CPPFLAGS) -I$(FW_LPCUSB) \
		-I$(FW_COMMON) -DUBERTOOTH_ONE -DFW_HOST_TEST \
		-Wno-unused-variable -Wno-pointer-to-int-cast \
		-o $@ test_le_phy_logic.c $(FW_RXTX)/queue.c \
		$(FW_RXTX)/bluetooth_le.c $(HOST_LIB)/ubertooth_le_frag.c

# How long a low-energy capture waits for the rest of a packet. The two
# helpers the deadline is built from are pure logic in bluetooth_le.h, so they
# run here; stubs/ubertooth.h stands in for the hardware header that file
# includes, as it does for test_le_frag_shape. The wait itself is hardware
# code and is scanned there.
$(BINDIR)/test_le_rx_byte_wait: test_le_rx_byte_wait.c minunit.h \
		stubs/ubertooth.h $(FW_RXTX)/bluetooth_le.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs $(CPPFLAGS) -o $@ test_le_rx_byte_wait.c

# What reset_le() clears, field by field. It is in a hardware file, so this
# reads le_state_t out of bluetooth_le.h and the function out of
# bluetooth_rxtx.c and compares the two; FW_RXTX_DIR points at both.
$(BINDIR)/test_le_state_reset: test_le_state_reset.c minunit.h source_scan.h \
		$(FW_RXTX)/bluetooth_le.h $(FW_RXTX)/bluetooth_rxtx.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_le_state_reset.c

# How long a capture the target filter needs before it compares the address in
# it. The filter runs inside the capture loop, so it is modelled here and
# le_phy.c is scanned for the bounds the model uses, as test_le_phy_order.c
# scans the same file for the order of its PHY writes.
$(BINDIR)/test_le_filter_bounds: test_le_filter_bounds.c minunit.h \
		source_scan.h $(FW_RXTX)/le_phy.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_le_filter_bounds.c

# The fragmenting enqueue is pure logic, so ubertooth_usb.c is compiled into
# this test (as test_usb_queue.c does) and run for real, against the host's
# reassembly linked in beside it. stubs-usbq/ stands in for ubertooth.h and
# lpcusb's usbapi.h and comes first so it wins; usbhw_lpc.h is register
# #defines and prototypes, so the real one is used.
$(BINDIR)/test_le_frag_enqueue: test_le_frag_enqueue.c minunit.h \
		stubs-usbq/usbapi.h stubs-usbq/ubertooth.h stubs-usbq/watchdog.h \
		stubs/ubertooth.h \
		$(FW_RXTX)/ubertooth_usb.c $(FW_RXTX)/ubertooth_usb.h \
		$(FW_COMMON)/usb_irq.h \
		../../host/libubertooth/src/ubertooth_le_frag.c \
		../../host/libubertooth/src/ubertooth_le_frag.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-usbq $(CPPFLAGS) -I$(FW_LPCUSB) \
		-I$(FW_COMMON) -DFW_HOST_TEST \
		-o $@ test_le_frag_enqueue.c \
		../../host/libubertooth/src/ubertooth_le_frag.c

# grmdm_packet_mode() is a bit test in cc2400.h, so it runs here; the
# rx_generic() that uses it is hardware code, so the same test scans
# bluetooth_rxtx.c for the shape.
$(BINDIR)/test_grmdm_rx_generic: test_grmdm_rx_generic.c minunit.h \
		source_scan.h $(FW_COMMON)/cc2400.h $(FW_RXTX)/bluetooth_rxtx.c \
		$(HOST_LIB)/ubertooth_interface.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(FW_COMMON) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_grmdm_rx_generic.c

# The generic transmit request's handler and tx_generic() are hardware code, so
# the bounds on a transmit packet are exercised through fakes and the firmware's
# own shape is scanned, as test_frend does. generic_tx_packet and DMA_SIZE come
# from the shared interface header, already on the include path.
# -I../common is for cc2400.h, which has the radio's FIFO size: the bound the
# payload is cut into pieces by.
$(BINDIR)/test_tx_generic_bounds: test_tx_generic_bounds.c minunit.h \
		source_scan.h $(FW_RXTX)/bluetooth_rxtx.c $(FW_COMMON)/cc2400.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(FW_COMMON) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_tx_generic_bounds.c

# The e-go receive is hardware code as well -- do_rx() reads the SSP's data
# register -- so the bounds on a capture are exercised through fakes and the
# firmware's own shape is scanned, as the generic transmit test above does.
# The host tool's capture-length range is scanned too, so the two cannot
# disagree about what a board accepts.
$(BINDIR)/test_ego_rx_bounds: test_ego_rx_bounds.c minunit.h \
		source_scan.h $(FW_RXTX)/ego.c $(HOST_DFU)/ego_options.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DHOST_EGO_OPTIONS='"$(HOST_DFU)/ego_options.h"' \
		-o $@ test_ego_rx_bounds.c

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
$(BINDIR)/test_btbr_hop: test_btbr_hop.c minunit.h spec_hop_vectors.h \
		stubs-btbr/ubtbr/debug.h \
		$(FW_BTBR)/src/btphy/hop.c $(FW_BTBR)/src/codec/perm5.c \
		$(FW_BTBR)/include/ubtbr/hop.h $(FW_BTBR)/include/ubtbr/btphy.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-btbr -I$(FW_BTBR)/include $(CPPFLAGS) \
		-o $@ test_btbr_hop.c $(FW_BTBR)/src/btphy/hop.c \
		$(FW_BTBR)/src/codec/perm5.c

# The btbr image's receive task waits for a packet's bytes from a slot task in
# the timer interrupt, so the test drives those waits with a fake slot clock
# and a fake DMA controller: stubs-btbr-rx/ supplies a hardware-free
# ubertooth.h with both, plus an interrupt-mask stand-in, and stubs-btbr/ the
# diagnostics stand-in. rx_task.c is included into the test (see its header
# comment), because the waits and their callers are static. bitstream_proc.c is
# the byte-reversal table the task reads the DMA buffer through, and is pure
# logic, so the real one links. Its last case scans rx_task.c as text, so
# FW_BTBR_DIR points at the source.
$(BINDIR)/test_btbr_rx_deadline: test_btbr_rx_deadline.c minunit.h \
		source_scan.h stubs-btbr-rx/ubertooth.h \
		stubs-btbr-rx/ubtbr/system.h stubs-btbr/ubtbr/debug.h \
		stubs/ubertooth.h $(FW_BTBR)/src/btphy/rx_task.c \
		$(FW_BTBR)/src/codec/bitstream_proc.c \
		$(FW_BTBR)/include/ubtbr/rf.h \
		$(FW_BTBR)/include/ubtbr/btphy.h | $(BINDIR)
# rx_task.c carries unused locals and unused slot-task parameters of its own,
# and takes the address of a packed field to hand the codec a size to fill in.
# None of that is what this tests, and none of it is this change's.
	$(CC) $(CFLAGS) -Istubs-btbr-rx -Istubs-btbr -I$(FW_BTBR)/include \
		$(CPPFLAGS) -I$(FW_COMMON) -DFW_BTBR_DIR='"$(FW_BTBR)"' \
		-Wno-unused-parameter -Wno-unused-variable \
		-Wno-unused-but-set-variable -Wno-address-of-packed-member \
		-o $@ test_btbr_rx_deadline.c \
		$(FW_BTBR)/src/codec/bitstream_proc.c

# The btbr image's bitstream codec is pure logic, so the real one is linked
# here and its offsets are exercised at the sizes a long Classic packet
# reaches. stubs-btbr/ supplies the diagnostics stand-in, and the codec's own
# headers supply the chunk sizes and the slot clock. The last cases read the
# two syncword windows out of the scan and receive tasks as text -- both are
# hardware files -- so FW_BTBR_DIR points at the image.
# The fault handlers write the board's LEDs and active_msleep() writes the
# system timer, so both files are read as text (see the test's header comment).
$(BINDIR)/test_btbr_fault_blink: test_btbr_fault_blink.c minunit.h \
		source_scan.h $(FW_BTBR)/src/util/fault.c \
		$(FW_BTBR)/src/util/debug.c \
		$(HOST_LIB)/ubertooth_interface.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_BTBR_DIR='"$(FW_BTBR)"' \
		-o $@ test_btbr_fault_blink.c

$(BINDIR)/test_btbr_arithmetic: test_btbr_arithmetic.c minunit.h \
		source_scan.h stubs-btbr/ubtbr/debug.h \
		$(FW_BTBR)/src/codec/bitstream_proc.c \
		$(FW_BTBR)/src/btphy/scan_task.c $(FW_BTBR)/src/btphy/rx_task.c \
		$(FW_BTBR)/include/ubtbr/bb.h $(FW_BTBR)/include/ubtbr/codec.h \
		$(FW_BTBR)/include/ubtbr/btphy.h $(FW_BTBR)/include/ubtbr/rf.h \
		| $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-btbr -I$(FW_BTBR)/include $(CPPFLAGS) \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' \
		-o $@ test_btbr_arithmetic.c $(FW_BTBR)/src/codec/bitstream_proc.c

# How many air bits a 2/3 FEC payload occupies, which transmit, receive and
# the packet's air size all have to agree on. codec.c is the only btbr source
# that holds all three, and it is pure logic, so the real one is linked here
# on top of bitstream_proc.c and whole packets are encoded and decoded through
# it. stubs-btbr/ supplies the diagnostics stand-in.
$(BINDIR)/test_btbr_codec_fec23: test_btbr_codec_fec23.c minunit.h \
		stubs-btbr/ubtbr/debug.h $(FW_BTBR)/src/codec/codec.c \
		$(FW_BTBR)/src/codec/bitstream_proc.c \
		$(FW_BTBR)/include/ubtbr/bb.h $(FW_BTBR)/include/ubtbr/codec.h \
		$(FW_BTBR)/include/ubtbr/btphy.h | $(BINDIR)
# codec.c takes the packet header in two calls that do not read it, and keeps
# a decode pointer one branch does not use. Neither is what this tests, and
# neither is this change's.
	$(CC) $(CFLAGS) -Istubs-btbr -I$(FW_BTBR)/include $(CPPFLAGS) \
		-Wno-unused-parameter -Wno-unused-variable \
		-o $@ test_btbr_codec_fec23.c $(FW_BTBR)/src/codec/codec.c \
		$(FW_BTBR)/src/codec/bitstream_proc.c

# The USB hardware layer reaches the controller only through the register
# macros in lpc17.h, so stubs-usb/ supplies a fake one and this links the real
# usbhw_lpc.c against it. -DLPC17xx picks that branch of the file; -Istubs-usb
# comes first so its lpc17.h wins over firmware/common's.
$(BINDIR)/test_usbhw_wait: test_usbhw_wait.c minunit.h stubs-usb/lpc17.h \
		stubs-usb/watchdog.h \
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
# FW_RXTX_DIR points at those sources too. The last section scans the btbr
# image instead -- its request dispatch, and its whole src/ tree for a second
# way to the host -- so FW_BTBR_DIR points at that image.
$(BINDIR)/test_usb_queue: test_usb_queue.c minunit.h source_scan.h \
		stubs-usbq/usbapi.h \
		stubs-usbq/ubertooth.h stubs/ubertooth.h \
		$(FW_RXTX)/ubertooth_usb.c $(FW_RXTX)/ubertooth_usb.h \
		$(FW_RXTX)/bluetooth_rxtx.c $(FW_RXTX)/le_phy.c $(FW_RXTX)/ego.c \
		$(FW_BTBR)/src/btctl/btctl.c \
		$(FW_BTBR)/src/btctl/vendor_request_handler.c \
		$(FW_BTBR)/src/drivers/ubertooth_usb.c \
		$(FW_COMMON)/usb_irq.h | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-usbq $(CPPFLAGS) -I$(FW_LPCUSB) \
		-I$(FW_COMMON) -DFW_HOST_TEST -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' \
		-DFW_COMMON_DIR='"$(FW_COMMON)"' \
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

# app_image.h reads no hardware, so the check that decides whether the
# bootloader starts an application compiles here as it is. The jump it guards
# is two instructions of assembly, so the test also reads bootloader.cpp as
# text to show the check is in front of them. It also reads flash.h and the
# host's dfu.h for the two constants the write order depends on, so all three
# sources are prerequisites, or editing one would leave the scan that guards
# it unrun.
$(BINDIR)/test_bootloader_app_image: test_bootloader_app_image.cpp \
		minunit.h source_scan.h $(FW_BOOT)/app_image.h \
		$(FW_BOOT)/bootloader.cpp $(FW_BOOT)/flash.h \
		$(HOST_DFU)/dfu.h | $(BINDIR)
	$(CXX) $(CXXFLAGS) -I$(FW_BOOT) $(CPPFLAGS) \
		-DFW_BOOTLOADER_DIR='"$(FW_BOOT)"' -DHOST_DFU_DIR='"$(HOST_DFU)"' \
		-o $@ test_bootloader_app_image.cpp
# Seven pieces of arithmetic that are wrong but that nothing in the tree can
# reach. Every one of them lives in a translation unit that talks to hardware,
# so this test links no firmware source: it checks each contract on its own and
# scans the sources below to show they are still written that way. Every
# scanned source is a prerequisite, or editing one would leave the scan that
# guards it unrun.
$(BINDIR)/test_latent_arithmetic: test_latent_arithmetic.c minunit.h \
		source_scan.h $(FW_RXTX)/bluetooth_rxtx.c $(FW_RXTX)/ego.c \
		$(FW_RXTX)/le_phy.c $(FW_COMMON)/ubertooth.c \
		$(FW_COMMON)/cc2400.h \
		$(FW_BTBR)/src/btphy/rx_task.c \
		$(HOST_LIB)/ubertooth_interface.h | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' -DFW_COMMON_DIR='"$(FW_COMMON)"' \
		-o $@ test_latent_arithmetic.c

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

# Which values the modulation and channel requests accept. Both cases write
# radio state, so they are modelled and the firmware's own shape is scanned,
# as test_vendor_request_bounds does for the bounds. -I../common is for
# cc2400.h, which holds the synthesizer's range.
$(BINDIR)/test_vendor_request_values: test_vendor_request_values.c minunit.h \
		source_scan.h $(FW_COMMON)/cc2400.h $(FW_RXTX)/bluetooth_rxtx.c \
		| $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(FW_COMMON) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-o $@ test_vendor_request_values.c

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
		stubs-btbr/ubtbr/debug.h stubs-btbr-usb/watchdog.h \
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

# The build string both images carry: this test scans their definitions and
# their request handlers as text, the way test_bootloader_flash_bounds puts
# the bootloader's part table against the host's copy. compile_info itself
# cannot be compiled here, because it is built from the generated version.h,
# which only exists once an image has been built.
$(BINDIR)/test_compile_info: test_compile_info.c minunit.h source_scan.h \
		$(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_BTBR)/src/btctl/vendor_request_handler.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' -o $@ test_compile_info.c

# The firmware info block every image carries. ../common/fw_info.c is compiled
# here with the host compiler, against the stub version.h in stubs-fwinfo/, so
# the bytes the firmware builds can be handed to the host's own parser; the
# rest of the test reads the linker script, the two request handlers, the three
# USB descriptors and common.mk as text, so FW_DIR and FW_BOOTLOADER_DIR point
# at the firmware tree as well.
$(BINDIR)/test_fw_info: test_fw_info.c minunit.h source_scan.h \
		stubs-fwinfo/version.h $(FW_COMMON)/fw_info.c \
		$(FW_COMMON)/sections.ld ../common.mk \
		$(FW_RXTX)/bluetooth_rxtx.c $(FW_RXTX)/ubertooth_usb.c \
		$(FW_BTBR)/src/btctl/vendor_request_handler.c \
		$(FW_BTBR)/src/drivers/ubertooth_usb.c \
		$(FW_BOOT)/bootloader.cpp | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-fwinfo $(CPPFLAGS) \
		-DFW_RXTX_DIR='"$(FW_RXTX)"' -DFW_BTBR_DIR='"$(FW_BTBR)"' \
		-DFW_COMMON_DIR='"$(FW_COMMON)"' -DFW_DIR='".."' \
		-DFW_BOOTLOADER_DIR='"$(FW_BOOT)"' \
		-o $@ test_fw_info.c $(FW_COMMON)/fw_info.c

# The range test and the repeater are hardware loops, so this scans
# cc2400_rangetest.c as text for the radio writes they need and for the shape
# of the repeater's turnaround delay, the way test_mode_dispatch.c scans the
# dispatch. It links nothing from the firmware.
# It also walks the firmware tree for a second copy of the range test, so
# FW_DIR points at firmware/ as well.
$(BINDIR)/test_rangetest_radio: test_rangetest_radio.c minunit.h source_scan.h \
		$(FW_RXTX)/cc2400_rangetest.c $(FW_COMMON)/ubertooth.c \
		$(FW_DIR)/assembly_test/assembly_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_COMMON_DIR='"$(FW_COMMON)"' -DFW_DIR='"$(FW_DIR)"' \
		-o $@ test_rangetest_radio.c

# A control transfer is only answered after its handler has returned, which is
# why a reset has to be deferred and waited for. The real control transfer
# handler shows that order, so it is linked here against fakes for the hardware
# layer below it -- no register file needed, unlike test_usbhw_wait. The waits
# themselves are hardware code, so both images' sources are scanned for them.
$(BINDIR)/test_reset_order: test_reset_order.c minunit.h source_scan.h \
		stubs-usb/watchdog.h \
		$(FW_LPCUSB)/usbcontrol.c $(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_BTBR)/src/btctl/btctl.c \
		$(FW_BTBR)/src/btctl/vendor_request_handler.c \
		$(FW_COMMON)/ubertooth.c | $(BINDIR)
# With the debug macros off, usbcontrol.c's stall path builds a dump the
# disabled DBG() would have printed and takes an endpoint status it then does
# not use. Neither is what this tests.
#
# -Istubs-usb is there for the breadcrumb marks usbcontrol.c makes: the real
# watchdog.h reaches the part's registers, and nothing here has a register
# file.
	$(CC) $(CFLAGS) -Istubs-usb -I$(FW_LPCUSB) $(CPPFLAGS) \
		-DFW_RXTX_DIR='"$(FW_RXTX)"' -DFW_BTBR_DIR='"$(FW_BTBR)"' \
		-DFW_COMMON_DIR='"$(FW_COMMON)"' \
		-Wno-unused-but-set-variable -Wno-unused-parameter \
		-o $@ test_reset_order.c $(FW_LPCUSB)/usbcontrol.c

# Where each image's object files land is decided by ../common.mk and the
# bootloader's own makefile, so this test reads those as text -- building both
# images in both orders and comparing them needs the ARM toolchain and several
# minutes. FW_DIR points it at firmware/, which it also walks for images.
$(BINDIR)/test_firmware_object_paths: test_firmware_object_paths.c minunit.h \
		$(FW_DIR)/common.mk $(FW_BOOT)/Makefile \
		$(FW_DIR)/tc13badge/Makefile | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFW_DIR='"$(FW_DIR)"' \
		-o $@ test_firmware_object_paths.c

# watchdog.c reaches the part only through the register macros, so
# stubs-watchdog/ubertooth.h replaces them with variables and the test drives
# them. It includes watchdog.c into its own translation unit, because the
# breadcrumb is a linker-placed object on the board and an ordinary one here.
# Its last cases scan both images for the places the feed is called from, and
# for the fault paths that mark where they were, so FW_RXTX_DIR, FW_BTBR_DIR
# and FW_COMMON_DIR point at them.
$(BINDIR)/test_watchdog: test_watchdog.c minunit.h source_scan.h \
		stubs-watchdog/ubertooth.h stubs/ubertooth.h \
		$(FW_COMMON)/watchdog.c $(FW_COMMON)/watchdog.h \
		$(FW_COMMON)/usb_irq.h $(FW_COMMON)/sections.ld \
		$(FW_RXTX)/ubertooth_usb.c $(FW_RXTX)/bluetooth_rxtx.c \
		$(FW_LPCUSB)/usbhw_lpc.c $(FW_LPCUSB)/usbcontrol.c \
		$(FW_RXTX)/ego.c $(FW_RXTX)/xmas.c $(FW_RXTX)/identify.c \
		$(FW_RXTX)/fault.c $(FW_COMMON)/usb_serial.c \
		$(FW_BTBR)/src/drivers/ubertooth_usb.c \
		$(FW_BTBR)/src/util/fault.c \
		$(FW_COMMON)/LPC17xx_Interrupts.c \
		$(FW_BTBR)/src/btctl/main.c | $(BINDIR)
	$(CC) $(CFLAGS) -Istubs-watchdog -I$(FW_COMMON) $(CPPFLAGS) \
		-DFW_HOST_TEST -DFW_RXTX_DIR='"$(FW_RXTX)"' \
		-DFW_BTBR_DIR='"$(FW_BTBR)"' -DFW_COMMON_DIR='"$(FW_COMMON)"' \
		-DFW_BOOTLOADER_DIR='"$(FW_BOOT)"' \
		-o $@ test_watchdog.c

# Does the ToorCon 13 badge image still compile?
#
# That image is in no build. It is a Renesas R8C part, so building it needs an
# m32c-elf toolchain that nobody working on this repository has, and the other
# tests here can only read its sources as text. This compiles them for syntax
# only, with the host compiler, which is enough to catch a change that no
# longer parses or that names something nothing declares. It says nothing
# about the code the real toolchain would generate, and it runs nothing.
#
# It is a C check only: init.S and ivects.S are not read, and -fsyntax-only
# does not assemble, so the R8C instructions in the sources' asm() statements
# are not checked either.
#
# r8c2l.h is the part's register header, written by the badge's own mkports
# from r8c2l.io, so the names the sources use are the real ones. It is
# generated into $(TC13_GEN) rather than into the badge's directory, so that
# running the tests leaves the source tree as it was and `make test_clean`
# takes the generated files with it.
#
# The first step is why that needs a guard. tc13badge.h asks for the header as
# #include "r8c2l.h", and a quoted include is looked for beside the file that
# asks for it before any -I directory, so an r8c2l.h left in the badge's
# directory by a real build of the image would be the one compiled -- whatever
# the -I order says -- and an old one could pass or fail for its own reasons.
# The badge's makefile does generate exactly that file there, and .gitignore
# hides it, so the rule refuses to run rather than quietly check the wrong
# header. `make -C ../tc13badge clean` clears it.
#
# CPPFLAGS is left out because nothing in it is the badge's: it points at the
# shipped image and the host library, and the badge includes only its own
# headers, one of which is a cc2400.h of its own.
#
# Two things about this part have to be put aside to use a host compiler at
# all, and neither is about the code being checked:
#
#   - __attribute__ is defined away. The three interrupt handlers are marked
#     __attribute__((interrupt)), meaning the R8C's interrupt frame; a host
#     compiler either ignores the name or, on x86, rejects a handler that
#     takes no arguments.
#   - main is renamed. The badge's entry point returns void, as a freestanding
#     one may, and a host compiler holds main to returning int.
#
# -std=gnu99 is the dialect the rest of this file compiles in, named here
# rather than left to each compiler's default, which has changed between
# releases. The gnu part of it is load-bearing: mkports writes an anonymous
# struct inside a union for each register's bits, which plain c99 does not
# have. It pins the dialect and nothing more -- without -Werror the two host
# compilers still disagree about how hard to be on some mistakes, so the
# Linux run can warn where the macOS one fails -- and either way the badge is
# compiled on both.
#
# No -Wall or -Wextra: the badge's C carries warnings of long standing in code
# nobody here can test, and which warnings appear depends on the host
# compiler, while whether the sources still parse does not.
$(BINDIR)/tc13badge_syntax.stamp: $(FW_BADGE)/tc13badge.c $(FW_BADGE)/toorcon.c \
		$(FW_BADGE)/tc13badge.h $(FW_BADGE)/cc2400.h \
		$(FW_BADGE)/bitmaps.h $(FW_BADGE)/r8c2l.io $(FW_BADGE)/mkports \
		| $(BINDIR)
	@if [ -e $(FW_BADGE)/r8c2l.h ]; then \
		echo "error: firmware/tc13badge/r8c2l.h exists and would be compiled instead of the one this check generates; run 'make -C firmware/tc13badge clean'" >&2; \
		exit 1; \
	fi
	mkdir -p $(TC13_GEN)
	perl $(FW_BADGE)/mkports $(FW_BADGE)/r8c2l.io \
		$(TC13_GEN)/r8c2l.h $(TC13_GEN)/r8c2l.inc
	$(CC) -fsyntax-only -std=gnu99 '-D__attribute__(x)=' -Dmain=tc13badge_main \
		-I$(TC13_GEN) -I$(FW_BADGE) \
		$(FW_BADGE)/tc13badge.c $(FW_BADGE)/toorcon.c
	touch $@

$(BINDIR):
	mkdir -p $(BINDIR)

# Build and run every test; stop on the first failure. The badge's syntax
# check is a prerequisite rather than one of TESTS, because it is a compile
# with nothing to run afterwards.
test: $(addprefix $(BINDIR)/,$(TESTS)) $(BINDIR)/tc13badge_syntax.stamp
	@fail=0; for t in $(addprefix $(BINDIR)/,$(TESTS)); do \
		echo "== running $$t =="; \
		$$t || fail=1; \
		echo; \
	done; \
	if [ $$fail -ne 0 ]; then echo "UNIT TESTS FAILED"; exit 1; fi; \
	echo "ALL UNIT TESTS PASSED"

clean:
	rm -rf $(BINDIR)
