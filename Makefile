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
MIN_MACOS     := 12.0
BUILD_DIR     := build
DEXT_DIR      := $(BUILD_DIR)/$(DRIVER_NAME).dext
DEXT_CONTENTS := $(DEXT_DIR)/Contents

# =============================================================================
# Toolchain
# =============================================================================

CXX           := clang++
XCRUN         := xcrun
SDK_PATH      := $(shell $(XCRUN) --sdk macosx --show-sdk-path 2>/dev/null)

# If we're on macOS with Xcode, use the proper sysroot
ifneq ($(SDK_PATH),)
SYSROOT_FLAGS  := --sysroot $(SDK_PATH)
FRAMEWORK_PATH := $(SDK_PATH)/System/Library/Frameworks
FW_FLAGS       := -iframework $(FRAMEWORK_PATH)
else
# Fallback for Linux CI (tests only — dext build requires macOS)
SYSROOT_FLAGS  :=
FW_FLAGS       :=
endif

# =============================================================================
# Compiler flags
# =============================================================================

CXXFLAGS := \
    -std=c++17 \
    -target $(ARCH)-apple-macos$(MIN_MACOS) \
    $(SYSROOT_FLAGS) \
    $(FW_FLAGS) \
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
# Linker flags
# =============================================================================

LDFLAGS := \
    -target $(ARCH)-apple-macos$(MIN_MACOS) \
    $(SYSROOT_FLAGS) \
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
ifeq ($(SDK_PATH),)
	$(warning macOS SDK not found — dext build requires Xcode on macOS.)
	$(warning Run 'make tests' to build and run unit tests only.)
endif

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

lint:
	clang-tidy \
	    $(DRIVER_SRCS) \
	    -- $(CXXFLAGS) $(INCLUDES) 2>&1 | head -100

# =============================================================================
# Clean
# =============================================================================

clean:
	rm -rf $(BUILD_DIR)
	@echo "Clean complete."
