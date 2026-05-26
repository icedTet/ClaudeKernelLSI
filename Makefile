# =============================================================================
# Makefile — LSI9300Driver DriverKit Extension for macOS on Apple Silicon
#
# Targets:
#   all        Build the driver extension bundle (default)
#   tests      Build and run unit tests (Linux CI or macOS userspace)
#   clean      Remove build artifacts
#   install    Install the dext via systemextensionsctl (macOS only)
#   sign       Code-sign the dext bundle (requires TEAM= variable)
#   lint       Run clang-tidy over the source tree
#
# Usage:
#   make                          # Build dext (macOS with Xcode)
#   make tests                    # Run unit tests (any platform)
#   make sign TEAM=A1B2C3D4E5     # Sign with your Developer ID team
#   make install                  # Load the driver (requires SIP disabled)
#
# Copyright (c) 2024 ClaudeKernelLSI Project.
# SPDX-License-Identifier: BSD-2-Clause
# =============================================================================

DRIVER_NAME   := LSI9300Driver
BUNDLE_ID     := com.claudekernellsi.driver.LSI9300Driver
ARCH          := arm64
BUILD_DIR     := build
DEXT_DIR      := $(BUILD_DIR)/$(DRIVER_NAME).dext
DEXT_CONTENTS := $(DEXT_DIR)/Contents

# =============================================================================
# Toolchain — DriverKit SDK
#
# DriverKit extensions MUST be compiled against the DriverKit SDK, not the
# macOS SDK.  PCIDriverKit/ and SCSIControllerDriverKit/ headers do not exist
# in the macOS SDK.
#
# SDK path: xcrun --sdk driverkit --show-sdk-path
# Target triple: arm64-apple-driverkit<version>   (NOT arm64-apple-macos*)
# =============================================================================

CXX           := clang++
XCRUN         := xcrun

# Resolve the DriverKit SDK (requires Xcode 12+)
DK_SDK_PATH   := $(shell $(XCRUN) --sdk driverkit --show-sdk-path 2>/dev/null)
DK_VERSION    := $(shell $(XCRUN) --sdk driverkit --show-sdk-version 2>/dev/null)

ifneq ($(DK_SDK_PATH),)
  DK_TARGET     := $(ARCH)-apple-driverkit$(DK_VERSION)
  # DriverKit 22+ (Xcode 14+) reorganised the SDK: extension-side frameworks
  # now live under System/DriverKit/System/Library/Frameworks rather than the
  # old System/Library/Frameworks.  The old path still exists in newer SDKs
  # but contains host-side stubs that lack OSDeclareDefaultStructors and the
  # virtual-method declarations needed by dext authors.  Use wildcard to pick
  # the correct path at build time, falling back to the legacy layout.
  _DK_FW_DEXT   := $(DK_SDK_PATH)/System/DriverKit/System/Library/Frameworks
  _DK_FW_LEGACY := $(DK_SDK_PATH)/System/Library/Frameworks
  DK_FW_PATH    := $(if $(wildcard $(_DK_FW_DEXT)/DriverKit.framework),$(_DK_FW_DEXT),$(_DK_FW_LEGACY))
  # -isysroot sets the compiler header search root (not --sysroot, which is
  # a linker flag and triggers -Wincompatible-sysroot when targeting DriverKit).
  # -iframework / -F add the extension SDK framework directory so that
  # #include <DriverKit/DriverKit.h>, <PCIDriverKit/…>, and
  # <SCSIControllerDriverKit/…> all resolve to the dext-side headers.
  DK_COMPILE    := -isysroot $(DK_SDK_PATH) -iframework $(DK_FW_PATH)
  DK_LINK       := -isysroot $(DK_SDK_PATH) -F$(DK_FW_PATH)
else
  # Not on macOS / no Xcode — unit-test target still works without this.
  DK_TARGET     := $(ARCH)-apple-driverkit22.0
  DK_COMPILE    :=
  DK_LINK       :=
endif

# =============================================================================
# Compiler flags  (DriverKit target)
# =============================================================================

CXXFLAGS := \
    -std=c++17 \
    -target $(DK_TARGET) \
    $(DK_COMPILE) \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Wno-unused-parameter \
    -fno-exceptions \
    -fno-rtti \
    -O2 \
    -g \
    -DDRIVER_BUNDLE_ID=\"$(BUNDLE_ID)\"

INCLUDES := -I LSI9300Driver

# =============================================================================
# Linker flags  (DriverKit target)
# =============================================================================

LDFLAGS := \
    -target $(DK_TARGET) \
    $(DK_LINK) \
    -framework DriverKit \
    -framework PCIDriverKit \
    -framework SCSIControllerDriverKit

# =============================================================================
# Sources
# =============================================================================

DRIVER_SRCS := \
    LSI9300Driver/LSI9300Driver.cpp \
    LSI9300Driver/MPT3IOC.cpp

DRIVER_OBJS := $(patsubst LSI9300Driver/%.cpp, $(BUILD_DIR)/%.o, $(DRIVER_SRCS))

DRIVER_HEADERS := $(wildcard LSI9300Driver/*.h)

# =============================================================================
# Test sources  (unit tests, compile for host; no DriverKit SDK required)
# =============================================================================

TEST_HOST_CXX := clang++

# Use host architecture for tests (tests run on CI, which may be x86_64 or arm64)
TEST_CXXFLAGS := \
    -std=c++17 \
    -DUNIT_TEST \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -O0 \
    -g \
    -I LSI9300Driver \
    -I Tests

TEST_SRCS := \
    Tests/MPT3ProtocolTests.cpp \
    Tests/MPT3ReplyQueueTests.cpp

TEST_BINS := \
    $(BUILD_DIR)/mpt3_protocol_tests \
    $(BUILD_DIR)/mpt3_reply_queue_tests

# =============================================================================
# Phony targets
# =============================================================================

.PHONY: all dext tests clean install sign lint check-sdk

# =============================================================================
# Default: build everything
# =============================================================================

all: check-sdk dext

check-sdk:
ifeq ($(DK_SDK_PATH),)
	$(error DriverKit SDK not found. Install Xcode (12+) and run:\
	  xcrun --sdk driverkit --show-sdk-path\
	  \nThe dext build requires macOS with Xcode. For unit tests only, run: make tests)
endif
	@echo "DriverKit SDK: $(DK_SDK_PATH)"
	@echo "DriverKit version: $(DK_VERSION)"
	@echo "Target triple: $(DK_TARGET)"
	@echo "Framework path: $(DK_FW_PATH)"

# =============================================================================
# Compile driver objects
# =============================================================================

$(BUILD_DIR)/%.o: LSI9300Driver/%.cpp $(DRIVER_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# =============================================================================
# Link driver binary
# =============================================================================

$(BUILD_DIR)/$(DRIVER_NAME): $(DRIVER_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

# =============================================================================
# Assemble .dext bundle
# =============================================================================

dext: $(BUILD_DIR)/$(DRIVER_NAME)
	@mkdir -p $(DEXT_CONTENTS)/MacOS
	cp $(BUILD_DIR)/$(DRIVER_NAME)       $(DEXT_CONTENTS)/MacOS/
	cp LSI9300Driver/Info.plist           $(DEXT_CONTENTS)/
	@echo "Bundle assembled at $(DEXT_DIR)"
	@echo "Run 'make sign TEAM=<your-team-id>' to code-sign."

# =============================================================================
# Code signing  (requires Apple Developer ID with DriverKit entitlements)
# =============================================================================

sign: dext
ifndef TEAM
	$(error TEAM is not set. Usage: make sign TEAM=A1B2C3D4E5)
endif
	codesign \
	    --sign "Apple Development: $(TEAM)" \
	    --entitlements LSI9300Driver/LSI9300Driver.entitlements \
	    --timestamp \
	    --options runtime \
	    --force \
	    $(DEXT_CONTENTS)/MacOS/$(DRIVER_NAME)
	codesign \
	    --sign "Apple Development: $(TEAM)" \
	    --entitlements LSI9300Driver/LSI9300Driver.entitlements \
	    --timestamp \
	    --options runtime \
	    --force \
	    $(DEXT_DIR)
	@echo "Signed: $(DEXT_DIR)"

# =============================================================================
# Install  (loads the system extension; requires macOS + proper signing)
# =============================================================================

install: sign
	systemextensionsctl install $(DEXT_DIR)

# =============================================================================
# Unit tests  (no macOS SDK needed; runs on Linux CI too)
# =============================================================================

tests: $(TEST_BINS)
	@echo "\n=== Running MPT3 Protocol Tests ==="
	$(BUILD_DIR)/mpt3_protocol_tests
	@echo "\n=== Running MPT3 Reply Queue Tests ==="
	$(BUILD_DIR)/mpt3_reply_queue_tests
	@echo "\n=== All tests complete ==="

$(BUILD_DIR)/mpt3_protocol_tests: Tests/MPT3ProtocolTests.cpp \
                                    Tests/TestStubs.h \
                                    $(DRIVER_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(TEST_HOST_CXX) $(TEST_CXXFLAGS) $< -o $@

$(BUILD_DIR)/mpt3_reply_queue_tests: Tests/MPT3ReplyQueueTests.cpp \
                                      Tests/TestStubs.h \
                                      $(DRIVER_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(TEST_HOST_CXX) $(TEST_CXXFLAGS) $< -o $@

# =============================================================================
# Lint
# =============================================================================

lint: check-sdk
	clang-tidy \
	    $(DRIVER_SRCS) \
	    -- $(CXXFLAGS) $(INCLUDES) 2>&1 | head -100

# =============================================================================
# Clean
# =============================================================================

clean:
	rm -rf $(BUILD_DIR)
	@echo "Clean complete."
