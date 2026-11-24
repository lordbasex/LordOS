/* nvmlmin.h — the few NVML types and signatures lord-nvgpu calls (layouts from NVIDIA's public nvml.h). Not a copy of
 * nvml.h: only what nvgpu_main.c resolves with dlsym and tests/nvgpu_fake_nvml.c exports. */
#ifndef LORD_MON_NVMLMIN_H
#define LORD_MON_NVMLMIN_H

typedef int nvmlReturn_t;
typedef struct nvmlDevice_st *nvmlDevice_t;

#define NVML_SUCCESS                0
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_TEMPERATURE_GPU        0
#define NVML_CLOCK_GRAPHICS         0
#define NVML_CLOCK_MEM              2
#define NVML_VALUE_NOT_AVAILABLE_ULL (~0ULL)

typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;
typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
typedef struct {
    char busIdLegacy[16];
    unsigned int domain, bus, device, pciDeviceId, pciSubSystemId;
    char busId[32];
} nvmlPciInfo_t;
/* nvmlProcessInfo_t as the _v2/_v3 calls fill it */
typedef struct { unsigned int pid; unsigned long long usedGpuMemory; unsigned int gpuInstanceId, computeInstanceId; } nvmlProcessInfo_t;
typedef struct { unsigned int pid; unsigned long long timeStamp; unsigned int smUtil, memUtil, encUtil, decUtil; } nvmlProcessUtilizationSample_t;

#endif
