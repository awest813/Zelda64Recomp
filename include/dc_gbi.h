#ifndef __DC_GBI_H__
#define __DC_GBI_H__

#ifdef DREAMCAST

#include <cstdint>

struct OSTask;

namespace dreamcast::gbi {

// Software F3DZEX2 display list interpreter for the Dreamcast port.
// Rasterizes into N64 RDRAM; presentation is handled by the PVR framebuffer blit.
class Interpreter {
public:
    Interpreter();
    ~Interpreter();

    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    void reset();
    void process_display_list(uint8_t* rdram, const OSTask* task);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace dreamcast::gbi

#endif // DREAMCAST

#endif // __DC_GBI_H__
