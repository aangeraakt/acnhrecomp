// NVN's alpha test (nvnColorStateSetAlphaTest, nvnCommandBufferSetAlphaRef): Vulkan has none, and Ryujinx's
// translator leaves it to the host's fixed function (its support buffer's alpha test field is declared, not read). The
// fragment module gets it here: before each return of the entry point, the alpha of colour output 0 is compared with
// the reference and the fragment discarded when the comparison fails. The map app's land is drawn through a dot
// pattern with an alpha test: without it the pattern's transparent gaps covered the land with sea (docs/STATUS.md).
#include <cstring>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpEntryPoint = 15, OpTypeBool = 20, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpConstant = 43,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpDecorate = 71, OpCompositeExtract = 81,
    OpFOrdEqual = 180, OpFOrdNotEqual = 182, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186,
    OpFOrdLessThanEqual = 188, OpFOrdGreaterThanEqual = 190, OpSelectionMerge = 247, OpLabel = 248,
    OpBranchConditional = 250, OpKill = 252, OpReturn = 253, DecorationLocation = 30, StorageClassOutput = 3,
};

}  // namespace

// `func`: NVNalphaFunc, 1 never .. 7 gequal (8 always is no test); `ref` the reference.
bool patch_alpha_test(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, uint32_t func, float ref) {
    static const uint32_t compare[8] = {0, 0, OpFOrdLessThan, OpFOrdEqual, OpFOrdLessThanEqual, OpFOrdGreaterThan,
                                        OpFOrdNotEqual, OpFOrdGreaterThanEqual};
    if (in.size() < 5 || func < 1 || func > 7) return false;
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
    uint32_t bound = in[3], entry = 0, f32 = 0, boolean = 0;
    std::vector<uint32_t> location0;  // ids decorated Location 0 (the output among them)
    uint32_t out_var = 0, out_type = 0;
    size_t first_function = in.size();
    std::vector<std::pair<uint32_t, uint32_t>> pointers;  // Output pointer id -> pointee
    std::vector<std::pair<uint32_t, uint32_t>> vec4;      // vec4 of f32 ids
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpEntryPoint && n.len >= 3 && !entry) entry = w[2];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation && w[3] == 0) location0.push_back(w[1]);
        else if (n.op == OpTypeBool && n.len == 2) boolean = w[1];
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && f32 && w[2] == f32 && w[3] == 4) vec4.push_back({w[1], 0});
        else if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageClassOutput) pointers.push_back({w[1], w[3]});
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageClassOutput && !out_var) {
            bool loc0 = false;
            for (uint32_t id : location0) loc0 |= id == w[2];
            if (!loc0) continue;
            for (auto& [ptr, pointee] : pointers)
                if (ptr == w[1])
                    for (auto& v : vec4)
                        if (v.first == pointee) out_var = w[2], out_type = pointee;
        } else if (n.op == OpFunction && first_function == in.size()) first_function = n.at;
    }
    if (!entry || !out_var || first_function == in.size()) return false;
    std::vector<uint32_t> types;  // after the existing types
    if (!boolean) boolean = bound++, types.insert(types.end(), {(2u << 16) | OpTypeBool, boolean});
    const uint32_t ref_id = bound++;
    uint32_t ref_bits;
    std::memcpy(&ref_bits, &ref, 4);
    types.insert(types.end(), {(4u << 16) | OpConstant, f32, ref_id, ref_bits});
    std::vector<uint32_t> body;
    bool in_entry = false;
    int patched = 0;
    for (const Inst& n : insts) {
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        if (n.op == OpFunction) in_entry = w[2] == entry;
        else if (n.op == OpFunctionEnd) in_entry = false;
        if (in_entry && n.op == OpReturn) {
            if (func == 1) {  // never: every fragment discarded
                body.push_back((1u << 16) | OpKill);
                patched++;
                continue;
            }
            const uint32_t kill = bound++, ret = bound++, color = bound++, alpha = bound++, pass = bound++;
            body.insert(body.end(), {(4u << 16) | OpLoad, out_type, color, out_var});
            body.insert(body.end(), {(5u << 16) | OpCompositeExtract, f32, alpha, color, 3});
            body.insert(body.end(), {(5u << 16) | compare[func], boolean, pass, alpha, ref_id});
            body.insert(body.end(), {(3u << 16) | OpSelectionMerge, ret, 0});
            body.insert(body.end(), {(4u << 16) | OpBranchConditional, pass, ret, kill});
            body.insert(body.end(), {(2u << 16) | OpLabel, kill, (1u << 16) | OpKill});
            body.insert(body.end(), {(2u << 16) | OpLabel, ret, (1u << 16) | OpReturn});
            patched++;
            continue;
        }
        body.insert(body.end(), w, w + n.len);
    }
    if (!patched) return false;
    out.assign(in.begin(), in.begin() + first_function);
    out.insert(out.end(), types.begin(), types.end());
    out.insert(out.end(), body.begin(), body.end());
    out[3] = bound;
    return true;
}

}  // namespace nx::nvn
