# Host unit tests for components/ert_scm (no ESPHome or hardware needed).
#   make -C tests        build and run with AddressSanitizer + UBSan
#   make -C tests clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
SRC_DIR := ../components/ert_scm
BUILD := build
TESTS := $(BUILD)/scm_decoder_test $(BUILD)/scm_format_test $(BUILD)/usage_history_test $(BUILD)/rssi_tracker_test

.PHONY: test clean
test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; echo; done

$(BUILD)/scm_decoder_test: scm_decoder_test.cpp test_harness.h $(SRC_DIR)/scm_decoder.cpp $(SRC_DIR)/scm_decoder.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) scm_decoder_test.cpp $(SRC_DIR)/scm_decoder.cpp -o $@

$(BUILD)/scm_format_test: scm_format_test.cpp test_harness.h $(SRC_DIR)/scm_format.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) scm_format_test.cpp -o $@

$(BUILD)/usage_history_test: usage_history_test.cpp test_harness.h $(SRC_DIR)/usage_history.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) usage_history_test.cpp -o $@

$(BUILD)/rssi_tracker_test: rssi_tracker_test.cpp test_harness.h $(SRC_DIR)/rssi_tracker.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) rssi_tracker_test.cpp -o $@

clean:
	rm -rf $(BUILD)
