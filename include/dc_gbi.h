#ifndef __DC_GBI_H__
#define __DC_GBI_H__

#ifdef DREAMCAST

#include <cstdint>

struct OSTask;

namespace dreamcast::pvr {
class Renderer;
}

namespace dreamcast::tex {
class Cache;
}

namespace dreamcast::gbi {

// F3DZEX2 display list high-level emulator for the Dreamcast port.
// Gfx commands update RSP state; triangles and fill rects are submitted to the
// PVR backend (SM64-style) instead of software-rasterizing into RDRAM.
class Interpreter {
public:
    Interpreter();
    ~Interpreter();

    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    void reset();
    void set_renderer(pvr::Renderer* renderer);
    void set_texture_cache(tex::Cache* cache);
    void process_display_list(uint8_t* rdram, const OSTask* task);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace dreamcast::gbi

#endif // DREAMCAST

#endif // __DC_GBI_H__
