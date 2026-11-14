CXX      ?= g++
CLANG    ?= clang
CXXFLAGS := -O2 -g -Wall -std=c++17
SPDLOG_INC ?= /usr/include
LDFLAGS  := -lbpf -lelf -lz -lpthread

BPF_CFLAGS := -O2 -g -target bpf -D__TARGET_ARCH_arm64 \
              -I/usr/include/$(shell uname -m)-linux-gnu

TARGET := rkperf
BPFOBJ := rkperf.bpf.o
BLOB   := bpf_blob.o

all: $(TARGET)

# 1) BPF 程序
$(BPFOBJ): src/rkperf.bpf.c
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

# 2) 把 BPF 对象塞进 ELF
$(BLOB): $(BPFOBJ)
	ld -r -b binary $< -o $@

# 3) 用户态程序（需要 spdlog 头文件）
$(TARGET): src/rkperf.cpp $(BLOB)
	$(CXX) $(CXXFLAGS) -I$(SPDLOG_INC) src/rkperf.cpp $(BLOB) -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET) $(BPFOBJ) $(BLOB)

.PHONY: all clean
