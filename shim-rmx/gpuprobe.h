#pragma once
#include <stdint.h>

/* dtb: the device tree ABL handed the shim; used to read /reserved-memory/pil_gpu_region. */
void gpuprobe_set_dtb(uintptr_t dtb);
void gpuprobe_run(void (*save)(const char *));
