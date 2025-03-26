CC := g++
NVCC := /usr/local/cuda-12.8/bin/nvcc
CUDA_PATH ?= /usr/local/cuda-12.8

OBJ_DIR := obj

CCFLAGS := -march=native -mtune=native -O3 -I$(CUDA_PATH)/include
NVCCFLAGS := -O3 -gencode=arch=compute_89,code=compute_89 -gencode=arch=compute_86,code=compute_86 -gencode=arch=compute_75,code=compute_75 -gencode=arch=compute_61,code=compute_61
LDFLAGS := -L$(CUDA_PATH)/lib64 -lcudart -pthread

CPU_SRC := RCKangaroo.cpp GpuKang.cpp Ec.cpp utils.cpp
GPU_SRC := RCGpuCore.cu

CPP_OBJECTS := $(addprefix $(OBJ_DIR)/, $(notdir $(CPU_SRC:.cpp=.o)))
CU_OBJECTS := $(addprefix $(OBJ_DIR)/, $(notdir $(GPU_SRC:.cu=.o)))

TARGET := rckangaroo

all: $(TARGET)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(TARGET): $(CPP_OBJECTS) $(CU_OBJECTS)
	$(CC) $(CCFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CC) $(CCFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.cu | $(OBJ_DIR)
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

clean:
	@echo Cleaning...
	rm -f $(CPP_OBJECTS) $(CU_OBJECTS)
