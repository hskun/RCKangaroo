CXX := g++
CUDA_PATH ?= /usr/local/cuda
NVCC := $(CUDA_PATH)/bin/nvcc

CXXFLAGS := -O3 -std=c++20 -I$(CUDA_PATH)/include -I. -Wall -Wextra -Wpedantic -Wno-unknown-pragmas
NVCCFLAGS := -O3 -std=c++20 -I. -gencode=arch=compute_89,code=compute_89
CUDA_LDFLAGS := -L$(CUDA_PATH)/lib64 -lcudart -lcuda -pthread -ldl -lrt
COMMON_LDFLAGS := -pthread -lrt
ZMQ_LDFLAGS := -lzmq

COMMON_CPP_OBJS := GpuKang.o Ec.o utils.o CallCubin.o
COMMON_CU_OBJS := RCGpuCore.o

TARGET_STANDALONE := rckangaroo
TARGET_WORKER := rckangaroo-worker
TARGET_SERVER := rckangaroo-server

all: $(TARGET_STANDALONE) $(TARGET_WORKER) $(TARGET_SERVER) rckangaroo-worker

$(TARGET_STANDALONE): RCKangaroo.o $(COMMON_CPP_OBJS) $(COMMON_CU_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CUDA_LDFLAGS)

$(TARGET_WORKER): rckangarooWorker.o $(COMMON_CPP_OBJS) $(COMMON_CU_OBJS) NetComm.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CUDA_LDFLAGS) $(ZMQ_LDFLAGS)

$(TARGET_SERVER): RCKangarooServer.o Ec.o utils.o NetComm.o StorageManager.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(COMMON_LDFLAGS) $(ZMQ_LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cu
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

clean:
	rm -f *.o
# 	rm -f *.o $(TARGET_STANDALONE) $(TARGET_WORKER) $(TARGET_SERVER)

.PHONY: all clean
