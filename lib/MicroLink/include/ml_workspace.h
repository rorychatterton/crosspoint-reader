#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*ml_workspace_alloc_fn)(size_t size, void *ctx);
typedef void (*ml_workspace_free_fn)(void *ptr, void *ctx);

typedef struct {
    uint8_t *data;
    size_t capacity;
    ml_workspace_free_fn free_fn;
    void *allocator_ctx;
} ml_workspace_t;

bool ml_workspace_reserve(ml_workspace_t *workspace, size_t capacity,
                          ml_workspace_alloc_fn alloc_fn,
                          ml_workspace_free_fn free_fn, void *allocator_ctx);
uint8_t *ml_workspace_get(ml_workspace_t *workspace, size_t required_capacity);
void ml_workspace_release(ml_workspace_t *workspace);

#ifdef __cplusplus
}
#endif
