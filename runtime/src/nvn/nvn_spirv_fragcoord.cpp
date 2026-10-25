// gl_FragCoord in the game's 16:9 units on a widened screen target (nvn_ultrawide.cpp). Ryujinx's render scale is one
// number per target (the resolution part): gl_FragCoord / scale ran to 1920 * k across a target k times wider than
// 16:9, and the game's passes that make a screen coordinate as gl_FragCoord.x * (1 / 1920) sampled past the edge from
// 1/k of the width on: the right quarter of the island smeared into horizontal bands at 21:9 (docs/STATUS.md). The
// fragment module's reads of gl_FragCoord.x and .y are multiplied by specialization constants 2 and 3 (1 / k on the
// wider axis, set per pipeline; 1 elsewhere) and added constants 4 and 5 (pixels: a squeezed UI draw, laid out over the
// 16:9 middle of the target, sees its coordinates from the middle's left edge).
#include <map>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpTypeFloat = 22, OpTypeVector = 23, OpConstant = 43, OpSpecConstant = 50, OpSpecConstantComposite = 51,
    OpFunction = 54, OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71,
    OpFAdd = 129, OpFMul = 133, DecorationBuiltIn = 11, DecorationSpecId = 1, BuiltInFragCoord = 15,
};

bool header_op(uint32_t op) {  // capabilities .. annotations: before the types
    switch (op) {
        case 17: case 10: case 11: case 14: case 15: case 16: case 330: case 3: case 4: case 5: case 6: case 7: case 8:
        case 71: case 72: case 73: case 74: case 75: return true;
        default: return false;
    }
}

}  // namespace

bool patch_fragcoord_scale(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    struct Inst {
        size_t at;
        uint32_t op, len;
    };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3], f32 = 0, v4f = 0, var = 0, one = 0;
    std::map<uint32_t, uint32_t> uint_constants;  // id -> value
    std::map<uint32_t, uint32_t> component;        // pointer into gl_FragCoord -> component
    size_t first_function = in.size(), first_type = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (first_type == in.size() && !header_op(n.op)) first_type = n.at;
        if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInFragCoord) var = w[1];
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && f32 && w[2] == f32 && w[3] == 4) v4f = w[1];
        else if (n.op == OpConstant && n.len == 4) {
            uint_constants[w[2]] = w[3];
            if (w[1] == f32 && w[3] == 0x3f800000u) one = w[2];
        } else if (n.op == OpFunction && first_function == in.size()) first_function = n.at;
        else if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len == 5 && var && w[3] == var) {
            auto c = uint_constants.find(w[4]);
            if (c != uint_constants.end() && c->second < 2) component[w[2]] = c->second;
        }
    }
    if (!var || !f32 || first_function == in.size()) return false;
    const uint32_t sx = bound++, sy = bound++, ox = bound++, oy = bound++;
    std::vector<uint32_t> types;  // after the existing types (f32 is among them)
    types.insert(types.end(), {(4u << 16) | OpSpecConstant, f32, sx, 0x3f800000u});
    types.insert(types.end(), {(4u << 16) | OpSpecConstant, f32, sy, 0x3f800000u});
    types.insert(types.end(), {(4u << 16) | OpSpecConstant, f32, ox, 0u});
    types.insert(types.end(), {(4u << 16) | OpSpecConstant, f32, oy, 0u});
    uint32_t sv = 0, ov = 0, zero = 0;
    auto vec_scale = [&]() {
        if (!sv && v4f) {
            if (!one) one = bound++, types.insert(types.end(), {(4u << 16) | OpConstant, f32, one, 0x3f800000u});
            zero = bound++, types.insert(types.end(), {(4u << 16) | OpConstant, f32, zero, 0u});
            sv = bound++, ov = bound++;
            types.insert(types.end(), {(7u << 16) | OpSpecConstantComposite, v4f, sv, sx, sy, one, one});
            types.insert(types.end(), {(7u << 16) | OpSpecConstantComposite, v4f, ov, ox, oy, zero, zero});
        }
        return sv;
    };
    std::vector<uint32_t> body;
    int patched = 0;
    for (const Inst& n : insts) {
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        if (n.op == OpLoad && n.len >= 4) {
            auto c = component.find(w[3]);
            uint32_t scale = c != component.end() && w[1] == f32 ? (c->second == 0 ? sx : sy)
                             : w[3] == var && w[1] == v4f   ? vec_scale()
                                                            : 0;
            uint32_t offset = scale == sx ? ox : scale == sy ? oy : ov;
            if (scale) {
                uint32_t raw = bound++, scaled = bound++;
                body.insert(body.end(), w, w + n.len);
                body[body.size() - n.len + 2] = raw;
                body.insert(body.end(), {(5u << 16) | OpFMul, w[1], scaled, raw, scale});
                body.insert(body.end(), {(5u << 16) | OpFAdd, w[1], w[2], scaled, offset});
                patched++;
                continue;
            }
        }
        body.insert(body.end(), w, w + n.len);
    }
    if (!patched) return false;
    // SpecId decorations with the annotations, the constants just before the first function (after every type).
    out.assign(in.begin(), in.begin() + first_type);
    out.insert(out.end(), {(4u << 16) | OpDecorate, sx, DecorationSpecId, 2});
    out.insert(out.end(), {(4u << 16) | OpDecorate, sy, DecorationSpecId, 3});
    out.insert(out.end(), {(4u << 16) | OpDecorate, ox, DecorationSpecId, 4});
    out.insert(out.end(), {(4u << 16) | OpDecorate, oy, DecorationSpecId, 5});
    out.insert(out.end(), in.begin() + first_type, in.begin() + first_function);
    out.insert(out.end(), types.begin(), types.end());
    out.insert(out.end(), body.begin(), body.end());
    out[3] = bound;
    return true;
}

}  // namespace nx::nvn
