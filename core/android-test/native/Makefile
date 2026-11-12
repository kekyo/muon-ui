CXX ?= g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2
RUNTIME := ../../android/runtime/src/main/cpp
OUT := .build

.PHONY: test clean
test: $(OUT)/utf-test
	$(OUT)/utf-test

$(OUT)/utf-test: muon_android_utf_test.cpp $(RUNTIME)/muon_android_utf.cpp $(RUNTIME)/muon_android_utf.h
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -I$(RUNTIME) muon_android_utf_test.cpp $(RUNTIME)/muon_android_utf.cpp -o $@

clean:
	rm -rf $(OUT)
