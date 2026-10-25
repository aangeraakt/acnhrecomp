// NaN texture coordinates read texel 0. Maxwell's texture unit turns a NaN coordinate into
// 0 when it converts it to fixed point; Apple's GPU does not (MoltenVK passes the NaN on). ACNH's tone mapping divides
// by the luminance, so a black pixel looks up its colour-grading LUT at NaN: on the Switch the LUT's first texel
// (black), on the Mac its last (white) - the tent's dark surroundings came out white. Before each sample a float
// coordinate is replaced by select(isnan(c), 0, c). NX_NAN_COORDS=0: off.
#include <cstdlib>
#include <map>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpExtInst = 12, OpTypeBool = 20, OpTypeFloat = 22, OpTypeVector = 23, OpConstantNull = 46, OpFunction = 54,
    OpLoad = 61, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81,
    OpImageSampleImplicitLod = 87, OpImageSampleProjDrefExplicitLod = 94, OpImageGather = 96, OpImageDrefGather = 97,
    OpConvertSToF = 111, OpConvertUToF = 112, OpBitcast = 124, OpFNegate = 127, OpFAdd = 129, OpFSub = 131,
    OpFMul = 133, OpFDiv = 136, OpVectorTimesScalar = 142, OpDot = 148, OpIsNan = 156, OpSelect = 169, OpPhi = 245,
};

}  // namespace

bool patch_nan_coords(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
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
    uint32_t bound = in[3], f32 = 0, boolean = 0;
    std::map<uint32_t, uint32_t> float_vectors;  // float vector type -> components
    std::map<uint32_t, uint32_t> bool_vectors;   // components -> bool vector type
    std::map<uint32_t, uint32_t> value_type;     // value -> its type (the ops a coordinate is made by)
    size_t first_function = in.size();
    bool any = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        switch (n.op) {
            case OpTypeFloat:
                if (n.len == 3 && w[2] == 32) f32 = w[1];
                break;
            case OpTypeBool: boolean = w[1]; break;
            case OpTypeVector:
                if (n.len == 4 && f32 && w[2] == f32) float_vectors[w[1]] = w[3];
                if (n.len == 4 && boolean && w[2] == boolean) bool_vectors[w[3]] = w[1];
                break;
            case OpFunction:
                if (first_function == in.size()) first_function = n.at;
                break;
            case OpExtInst: case OpLoad: case OpVectorShuffle: case OpCompositeConstruct: case OpCompositeExtract:
            case OpConvertSToF: case OpConvertUToF: case OpBitcast: case OpFNegate: case OpFAdd: case OpFSub:
            case OpFMul: case OpFDiv: case OpVectorTimesScalar: case OpDot: case OpSelect: case OpPhi:
                if (n.len >= 3) value_type[w[2]] = w[1];
                break;
            default:
                if ((n.op >= OpImageSampleImplicitLod && n.op <= OpImageSampleProjDrefExplicitLod) || n.op == OpImageGather ||
                    n.op == OpImageDrefGather)
                    any = true;
        }
    }
    if (!any || !f32 || first_function == in.size()) return false;

    // The types and constants the selects need, added before the first function.
    std::vector<uint32_t> globals;
    std::map<uint32_t, uint32_t> nulls;  // coordinate type -> its OpConstantNull
    auto bool_of = [&](uint32_t type) -> uint32_t {
        uint32_t count = type == f32 ? 1 : float_vectors.count(type) ? float_vectors[type] : 0;
        if (!count) return 0;
        if (!boolean) boolean = bound++, globals.insert(globals.end(), {(2u << 16) | OpTypeBool, boolean});
        if (count == 1) return boolean;
        uint32_t& v = bool_vectors[count];
        if (!v) v = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypeVector, v, boolean, count});
        return v;
    };
    auto null_of = [&](uint32_t type) {
        uint32_t& z = nulls[type];
        if (!z) z = bound++, globals.insert(globals.end(), {(3u << 16) | OpConstantNull, type, z});
        return z;
    };

    std::vector<uint32_t> body;  // the functions, with the selects in
    int patched = 0;
    for (size_t k = 0; k < insts.size(); k++) {
        const Inst& n = insts[k];
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        bool sample = (n.op >= OpImageSampleImplicitLod && n.op <= OpImageSampleProjDrefExplicitLod) ||
                      n.op == OpImageGather || n.op == OpImageDrefGather;
        if (sample && n.len >= 5) {
            auto t = value_type.find(w[4]);
            uint32_t b = t == value_type.end() ? 0 : bool_of(t->second);
            if (b) {
                uint32_t type = t->second, isnan = bound++, fixed = bound++;
                body.insert(body.end(), {(4u << 16) | OpIsNan, b, isnan, w[4]});
                body.insert(body.end(), {(6u << 16) | OpSelect, type, fixed, isnan, null_of(type), w[4]});
                size_t at = body.size();
                body.insert(body.end(), w, w + n.len);
                body[at + 4] = fixed;
                patched++;
                continue;
            }
        }
        body.insert(body.end(), w, w + n.len);
    }
    if (!patched) return false;
    out.assign(in.begin(), in.begin() + first_function);
    out.insert(out.end(), globals.begin(), globals.end());
    out.insert(out.end(), body.begin(), body.end());
    out[3] = bound;
    return true;
}

void nan_coords_module(std::vector<uint32_t>& code) {
    static const bool off = std::getenv("NX_NAN_COORDS") && std::getenv("NX_NAN_COORDS")[0] == '0';
    if (off) return;
    std::vector<uint32_t> patched;
    if (patch_nan_coords(code, patched)) code.swap(patched);
}

}  // namespace nx::nvn
