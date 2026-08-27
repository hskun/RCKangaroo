CC := g++-15
NVCC := /usr/local/cuda/bin/nvcc
CUDA_PATH ?= /usr/local/cuda

CCFLAGS := -O3 -std=c++17 -I$(CUDA_PATH)/include
NVCCFLAGS := -ccbin g++-15 -O3 -gencode=arch=compute_89,code=compute_89 -gencode=arch=compute_86,code=compute_86 -gencode=arch=compute_75,code=compute_75
LDFLAGS := -L$(CUDA_PATH)/lib64 -lcudart -pthread

CPU_WORKER_SRC := RCKangaroo.cpp GpuKang.cpp Ec.cpp utils.cpp NetComm.cpp
CPU_SERVER_SRC := RCKangarooServer.cpp Ec.cpp utils.cpp NetComm.cpp StorageManager.cpp
GPU_SRC := RCGpuCore.cu

WORKER_CPP_OBJECTS := $(CPU_WORKER_SRC:.cpp=.o)
SERVER_CPP_OBJECTS := $(CPU_SERVER_SRC:.cpp=.o)
CU_OBJECTS := $(GPU_SRC:.cu=.o)

TARGET_WORKER := rckangaroo-worker
TARGET_SERVER := rckangaroo-server

all: $(TARGET_WORKER) $(TARGET_SERVER)

$(TARGET_WORKER): $(WORKER_CPP_OBJECTS) $(CU_OBJECTS)
	$(CC) $(CCFLAGS) -o $@ $^ $(LDFLAGS) -lzmq

$(TARGET_SERVER): $(SERVER_CPP_OBJECTS)
	$(CC) $(CCFLAGS) -o $@ $^ $(LDFLAGS) -lzmq

%.o: %.cpp
	$(CC) $(CCFLAGS) -c $< -o $@

%.o: %.cu
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

clean:
	rm -f $(WORKER_CPP_OBJECTS) $(SERVER_CPP_OBJECTS) $(CU_OBJECTS)

