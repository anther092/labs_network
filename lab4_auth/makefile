CXX      := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread

BUILD    := build
SERVER   := server
CLIENT   := client

all: $(BUILD)/$(SERVER) $(BUILD)/$(CLIENT)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/$(SERVER): server.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/$(CLIENT): client.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean