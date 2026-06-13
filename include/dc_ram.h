#ifndef __DC_RAM_H__
#define __DC_RAM_H__

#ifdef DREAMCAST

#include <cstddef>

namespace dreamcast::ram {

// Initialize heap/stack accounting (call once during platform_init).
void init();

// Approximate free main RAM (mallinfo-based, SM64 DC style).
size_t free_bytes();
size_t used_heap_bytes();
size_t total_bytes();

// Log a one-line RAM summary to stdout.
void log_status(const char* tag = nullptr);

} // namespace dreamcast::ram

#endif // DREAMCAST

#endif // __DC_RAM_H__
