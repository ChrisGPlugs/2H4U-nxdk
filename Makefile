# nxdk build of 2H4UX (2H4U) for the original Xbox.
#
#   1. Get nxdk:  git clone --recursive https://github.com/XboxDev/nxdk.git
#   2. Build:     make NXDK_DIR=/path/to/nxdk
#
# Output: bin/ (default.xbe + data/) ready to copy to the Xbox, and 2H4UX.iso.

XBE_TITLE   = 2H4UX
TITLE_ID    = 32483455
ISO_NAME    = 2H4UX.iso

NXDK_DIR   ?= $(CURDIR)/../nxdk
NXDK_SDL    = y
NXDK_CXX    = y

SRCS        = $(filter-out %/platform_host.cpp,$(wildcard $(CURDIR)/src/*.cpp))
CXXFLAGS   += -D_XBOX -DNXDK -O2 -Wno-unused-variable -Wno-sign-compare \
              -Wno-writable-strings -Wno-deprecated-declarations

# nxdk's tools (nxdk-cc, nxdk-link, ...) live in $(NXDK_DIR)/bin.
export PATH := $(NXDK_DIR)/bin:$(PATH)

.DEFAULT_GOAL := release

include $(NXDK_DIR)/Makefile

release: $(OUTPUT_DIR)/default.xbe $(EXTRACT_XISO)
	@echo "[ TITLEID  ] $(TITLE_ID)"
	$(VE)python3 tools/set_titleid.py $(OUTPUT_DIR)/default.xbe $(TITLE_ID) $(QUIET)
	@echo "[ DATA     ] $(OUTPUT_DIR)/data"
	$(VE)rm -rf $(OUTPUT_DIR)/data && cp -r data $(OUTPUT_DIR)/data
	@echo "[ XISO     ] $(ISO_NAME)"
	$(VE)rm -f "$(ISO_NAME)" && $(EXTRACT_XISO) -c $(OUTPUT_DIR) "$(ISO_NAME)" $(QUIET)

.PHONY: release
