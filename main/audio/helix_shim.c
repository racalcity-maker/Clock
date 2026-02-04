#include <stdlib.h>
#include "esp_heap_caps.h"
#include "helix_memory.h"

// Helix allocators: use PSRAM if available, fallback to default heap.
void *helix_malloc(int size)
{
    void *p = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc((size_t)size, MALLOC_CAP_DEFAULT);
    }
    return p;
}

void helix_free(void *ptr)
{
    heap_caps_free(ptr);
}
