#include "ml_workspace.h"

#include <string.h>

bool ml_workspace_reserve(ml_workspace_t *workspace, size_t capacity,
                          ml_workspace_alloc_fn alloc_fn,
                          ml_workspace_free_fn free_fn, void *allocator_ctx) {
    if (!workspace || capacity == 0 || !alloc_fn || !free_fn || workspace->data) return false;
    uint8_t *data = (uint8_t *)alloc_fn(capacity, allocator_ctx);
    if (!data) return false;
    workspace->data = data;
    workspace->capacity = capacity;
    workspace->free_fn = free_fn;
    workspace->allocator_ctx = allocator_ctx;
    return true;
}

uint8_t *ml_workspace_get(ml_workspace_t *workspace, size_t required_capacity) {
    if (!workspace || !workspace->data || required_capacity > workspace->capacity) return NULL;
    return workspace->data;
}

void ml_workspace_release(ml_workspace_t *workspace) {
    if (!workspace || !workspace->data) return;
    workspace->free_fn(workspace->data, workspace->allocator_ctx);
    memset(workspace, 0, sizeof(*workspace));
}
