#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Find the first node whose name is `name` (e.g. "pil_gpu_region") and decode
 * its `reg` as one address+size pair. Handles 2/2 and 1/1 cell layouts.
 * Returns 0 if the node or the property is absent.
 */
int fdt_node_reg64(uintptr_t fdt, const char *name, uint64_t *addr, uint64_t *size);
