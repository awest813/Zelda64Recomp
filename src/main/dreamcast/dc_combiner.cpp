#ifdef DREAMCAST

#include "dc_combiner.h"

#include <algorithm>

namespace dreamcast::combiner {

namespace {

constexpr uint32_t G_CCMUX_COMBINED = 0;
constexpr uint32_t G_CCMUX_TEXEL0 = 1;
constexpr uint32_t G_CCMUX_TEXEL1 = 2;
constexpr uint32_t G_CCMUX_PRIMITIVE = 3;
constexpr uint32_t G_CCMUX_SHADE = 4;
constexpr uint32_t G_CCMUX_ENVIRONMENT = 5;
constexpr uint32_t G_CCMUX_1 = 6;
constexpr uint32_t G_CCMUX_NOISE = 7;
constexpr uint32_t G_CCMUX_0 = 31;
constexpr uint32_t G_CCMUX_TEXEL0_ALPHA = 8;
constexpr uint32_t G_CCMUX_TEXEL1_ALPHA = 9;
constexpr uint32_t G_CCMUX_PRIM_ALPHA = 10;
constexpr uint32_t G_CCMUX_SHADE_ALPHA = 11;
constexpr uint32_t G_CCMUX_ENV_ALPHA = 12;
constexpr uint32_t G_CCMUX_LOD_FRACTION = 13;
constexpr uint32_t G_CCMUX_PRIM_LOD_FRAC = 14;

struct Cycle {
    uint8_t a = 0;
    uint8_t b = 0;
    uint8_t c = 0;
    uint8_t d = 0;
};

uint8_t mul_u8(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>((static_cast<uint16_t>(a) * b) / 255u);
}

uint8_t sub_u8(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>(std::max<int>(static_cast<int>(a) - static_cast<int>(b), 0));
}

using ChannelFn = uint8_t (*)(const ColorSource&);

uint8_t channel_r(const ColorSource& c) { return c.r; }
uint8_t channel_g(const ColorSource& c) { return c.g; }
uint8_t channel_b(const ColorSource& c) { return c.b; }

uint8_t mux_rgb(uint32_t mux, const Inputs& in, const ColorSource& combined, ChannelFn channel) {
    switch (mux) {
    case G_CCMUX_COMBINED:
        return channel(combined);
    case G_CCMUX_TEXEL0:
        return channel(in.texel0);
    case G_CCMUX_TEXEL1:
        return channel(in.texel1);
    case G_CCMUX_PRIMITIVE:
        return channel(in.prim);
    case G_CCMUX_SHADE:
        return channel(in.shade);
    case G_CCMUX_ENVIRONMENT:
        return channel(in.env);
    case G_CCMUX_1:
        return 255;
    case G_CCMUX_0:
        return 0;
    default:
        return 0;
    }
}

uint8_t mux_alpha(uint32_t mux, const Inputs& in, const ColorSource& combined) {
    switch (mux) {
    case G_CCMUX_COMBINED:
        return combined.a;
    case G_CCMUX_TEXEL0:
    case G_CCMUX_TEXEL0_ALPHA:
        return in.texel0.a;
    case G_CCMUX_TEXEL1:
    case G_CCMUX_TEXEL1_ALPHA:
        return in.texel1.a;
    case G_CCMUX_PRIMITIVE:
    case G_CCMUX_PRIM_ALPHA:
        return in.prim.a;
    case G_CCMUX_SHADE:
    case G_CCMUX_SHADE_ALPHA:
        return in.shade.a;
    case G_CCMUX_ENVIRONMENT:
    case G_CCMUX_ENV_ALPHA:
        return in.env.a;
    case G_CCMUX_LOD_FRACTION:
        return in.lod_fraction;
    case G_CCMUX_PRIM_LOD_FRAC:
        return in.prim_lod_frac;
    case G_CCMUX_1:
        return 255;
    case G_CCMUX_0:
        return 0;
    default:
        return 0;
    }
}

uint8_t eval_cycle_channel(const Cycle& cycle, const Inputs& in, const ColorSource& combined, ChannelFn channel) {
    const uint8_t a = mux_rgb(cycle.a, in, combined, channel);
    const uint8_t b = mux_rgb(cycle.b, in, combined, channel);
    const uint8_t c = mux_rgb(cycle.c, in, combined, channel);
    const uint8_t d = mux_rgb(cycle.d, in, combined, channel);
    return static_cast<uint8_t>(std::min<int>(mul_u8(sub_u8(a, b), c) + d, 255));
}

ColorSource eval_cycle_rgb(const Cycle& cycle, const Inputs& in, const ColorSource& combined) {
    ColorSource out = combined;
    out.r = eval_cycle_channel(cycle, in, combined, channel_r);
    out.g = eval_cycle_channel(cycle, in, combined, channel_g);
    out.b = eval_cycle_channel(cycle, in, combined, channel_b);
    return out;
}

uint8_t eval_cycle_alpha(const Cycle& cycle, const Inputs& in, const ColorSource& combined) {
    const uint8_t a = mux_alpha(cycle.a, in, combined);
    const uint8_t b = mux_alpha(cycle.b, in, combined);
    const uint8_t c = mux_alpha(cycle.c, in, combined);
    const uint8_t d = mux_alpha(cycle.d, in, combined);
    return static_cast<uint8_t>(std::min<int>(mul_u8(sub_u8(a, b), c) + d, 255));
}

void decode_cycles(uint64_t combine_mode, Cycle& rgb0, Cycle& alpha0, Cycle& rgb1, Cycle& alpha1) {
    // combine_mode packs the two G_SETCOMBINE command words as (w1 << 32) | w0,
    // so w0 is the first command word (opcode in bits 24-31) and w1 the second.
    // Field positions follow the canonical gbi.h GCCc*w* macros.
    const uint32_t w0 = static_cast<uint32_t>(combine_mode & 0xFFFFFFFFu);
    const uint32_t w1 = static_cast<uint32_t>((combine_mode >> 32) & 0xFFFFFFFFu);

    // Color cycle 0: a/c live in w0, b/d in w1. The color c mux is 5 bits.
    rgb0.a = static_cast<uint8_t>((w0 >> 20) & 0x0F);
    rgb0.c = static_cast<uint8_t>((w0 >> 15) & 0x1F);
    rgb0.b = static_cast<uint8_t>((w1 >> 28) & 0x0F);
    rgb0.d = static_cast<uint8_t>((w1 >> 15) & 0x07);

    // Alpha cycle 0: a/c in w0, b/d in w1. All alpha muxes are 3 bits.
    alpha0.a = static_cast<uint8_t>((w0 >> 12) & 0x07);
    alpha0.c = static_cast<uint8_t>((w0 >> 9) & 0x07);
    alpha0.b = static_cast<uint8_t>((w1 >> 12) & 0x07);
    alpha0.d = static_cast<uint8_t>((w1 >> 9) & 0x07);

    // Color cycle 1: a/c in w0, b/d in w1.
    rgb1.a = static_cast<uint8_t>((w0 >> 5) & 0x0F);
    rgb1.c = static_cast<uint8_t>((w0 >> 0) & 0x1F);
    rgb1.b = static_cast<uint8_t>((w1 >> 24) & 0x0F);
    rgb1.d = static_cast<uint8_t>((w1 >> 6) & 0x07);

    // Alpha cycle 1: all four fields live in w1.
    alpha1.a = static_cast<uint8_t>((w1 >> 21) & 0x07);
    alpha1.c = static_cast<uint8_t>((w1 >> 18) & 0x07);
    alpha1.b = static_cast<uint8_t>((w1 >> 3) & 0x07);
    alpha1.d = static_cast<uint8_t>((w1 >> 0) & 0x07);
}

bool mux_uses_texture(uint32_t mux) {
    return mux == G_CCMUX_TEXEL0 || mux == G_CCMUX_TEXEL1
        || mux == G_CCMUX_TEXEL0_ALPHA || mux == G_CCMUX_TEXEL1_ALPHA;
}

} // anonymous namespace

uint32_t evaluate(uint64_t combine_mode, const Inputs& in, bool cycle2) {
    if (combine_mode == 0) {
        return (static_cast<uint32_t>(in.shade.a) << 24)
             | (static_cast<uint32_t>(in.shade.r) << 16)
             | (static_cast<uint32_t>(in.shade.g) << 8)
             | static_cast<uint32_t>(in.shade.b);
    }

    Cycle rgb0{};
    Cycle alpha0{};
    Cycle rgb1{};
    Cycle alpha1{};
    decode_cycles(combine_mode, rgb0, alpha0, rgb1, alpha1);

    ColorSource empty{};
    ColorSource combined = eval_cycle_rgb(rgb0, in, empty);
    combined.a = eval_cycle_alpha(alpha0, in, empty);

    if (cycle2) {
        combined = eval_cycle_rgb(rgb1, in, combined);
        combined.a = eval_cycle_alpha(alpha1, in, combined);
    }

    return (static_cast<uint32_t>(combined.a) << 24)
         | (static_cast<uint32_t>(combined.r) << 16)
         | (static_cast<uint32_t>(combined.g) << 8)
         | static_cast<uint32_t>(combined.b);
}

bool uses_texel0(uint64_t combine_mode) {
    if (combine_mode == 0) {
        return false;
    }
    Cycle rgb0{}, alpha0{}, rgb1{}, alpha1{};
    decode_cycles(combine_mode, rgb0, alpha0, rgb1, alpha1);
    const auto uses = [](const Cycle& c) {
        return mux_uses_texture(c.a) || mux_uses_texture(c.b) || mux_uses_texture(c.c) || mux_uses_texture(c.d);
    };
    return uses(rgb0) || uses(alpha0) || uses(rgb1) || uses(alpha1);
}

bool uses_texel1(uint64_t combine_mode) {
    if (combine_mode == 0) {
        return false;
    }
    Cycle rgb0{}, alpha0{}, rgb1{}, alpha1{};
    decode_cycles(combine_mode, rgb0, alpha0, rgb1, alpha1);
    const auto uses = [](const Cycle& c) {
        return c.a == G_CCMUX_TEXEL1 || c.b == G_CCMUX_TEXEL1 || c.c == G_CCMUX_TEXEL1 || c.d == G_CCMUX_TEXEL1
            || c.a == G_CCMUX_TEXEL1_ALPHA || c.b == G_CCMUX_TEXEL1_ALPHA || c.c == G_CCMUX_TEXEL1_ALPHA || c.d == G_CCMUX_TEXEL1_ALPHA;
    };
    return uses(rgb0) || uses(alpha0) || uses(rgb1) || uses(alpha1);
}

} // namespace dreamcast::combiner

#endif // DREAMCAST
