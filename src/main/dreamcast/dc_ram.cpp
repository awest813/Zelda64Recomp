// Main-RAM telemetry for Dreamcast hardware profiling.
//
// SM64 DC tracks free RAM from day one via mallinfo(); we do the same so
// playtests can spot OOM pressure before the game hard-crashes.

#ifdef DREAMCAST

#include "dc_ram.h"
#include "dreamcast_platform.h"

#include <cstdio>
#include <malloc.h>

extern "C" {
extern char _start;
extern char _end;
}

namespace dreamcast::ram {

namespace {

size_t g_stack_bytes = 0;

} // anonymous namespace

void init() {
    g_stack_bytes = static_cast<size_t>(&_end - &_start);
}

size_t total_bytes() {
    return DC_MAIN_RAM_SIZE;
}

size_t used_heap_bytes() {
    const struct mallinfo mi = mallinfo();
    return static_cast<size_t>(mi.uordblks);
}

size_t free_bytes() {
    const struct mallinfo mi = mallinfo();
    // fordblks is the free bytes in the heap arena on newlib/KOS.
    const size_t heap_free = static_cast<size_t>(mi.fordblks);
    if (heap_free > 0) {
        return heap_free;
    }
    // Fallback for platforms that only expose total-used accounting.
    const size_t used = used_heap_bytes() + g_stack_bytes;
    return total_bytes() > used ? total_bytes() - used : 0;
}

void log_status(const char* tag) {
    const struct mallinfo mi = mallinfo();
    fprintf(stdout,
            "[DC RAM%s] total=%zu KiB free~=%zu KiB heap_used=%zu KiB stack~=%zu KiB arena=%zu KiB\n",
            tag ? tag : "",
            total_bytes() / 1024,
            free_bytes() / 1024,
            used_heap_bytes() / 1024,
            g_stack_bytes / 1024,
            static_cast<size_t>(mi.arena) / 1024);
}

} // namespace dreamcast::ram

#endif // DREAMCAST
