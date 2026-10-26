// Diagnostics for ACNH bring-up (NX_ALLOC_TRACE=1): the sizes the game's StandardAllocators are set up with, and
// every allocation that fails, with the guest backtrace. Off by default: the wrappers then pass straight through.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <nx/runtime.h>

namespace nx {

void backtrace(CPU& c);

bool alloc_tracing() {
    static const bool on = std::getenv("NX_ALLOC_TRACE") != nullptr;
    return on;
}

namespace {

bool tracing() { return alloc_tracing(); }

uint64_t real(const char* name) {
    uint64_t a = find_symbol(name);
    if (!a) fatal("diag: %s not found", name);
    return a;
}

void Initialize(CPU& c) {  // (this, void* addr, size_t size)
    static uint64_t f = real("_ZN2nn3mem17StandardAllocator10InitializeEPvm");
    if (tracing()) log("alloc: StandardAllocator %llx initialized with %llx bytes at %llx (lr %s)",
                       (unsigned long long)c.x[0], (unsigned long long)c.x[2], (unsigned long long)c.x[1],
                       describe(c.x[30]).c_str());
    call(c, f);
}

void InitializeCache(CPU& c) {  // (this, void* addr, size_t size, bool thread cache)
    static uint64_t f = real("_ZN2nn3mem17StandardAllocator10InitializeEPvmb");
    if (tracing()) log("alloc: StandardAllocator %llx initialized with %llx bytes at %llx, cache %d (lr %s)",
                       (unsigned long long)c.x[0], (unsigned long long)c.x[2], (unsigned long long)c.x[1],
                       int(c.x[3] & 1), describe(c.x[30]).c_str());
    call(c, f);
}

template <const char* Name>
void Allocate(CPU& c) {
    static uint64_t f = real(Name);
    uint64_t self = c.x[0], size = c.x[1], lr = c.x[30];
    call(c, f);
    if (c.x[0] == 0 && tracing()) {
        log("alloc: StandardAllocator %llx: %llx bytes FAILED (lr %s)", (unsigned long long)self,
            (unsigned long long)size, describe(lr).c_str());
        uint64_t saved = c.x[30];
        c.x[30] = lr;
        backtrace(c);
        c.x[30] = saved;
    }
}

constexpr char kAlloc[] = "_ZN2nn3mem17StandardAllocator8AllocateEm";
constexpr char kAllocAligned[] = "_ZN2nn3mem17StandardAllocator8AllocateEmm";

// The thread-local "current heap" lookup of the engine (main+14505f8: GetTlsValue(slot) + 0xa8).
void GetTlsValue(CPU& c) {
    static uint64_t f = real("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE");
    uint64_t lr = c.x[30], slot = c.x[0];
    call(c, f);
    if (!tracing()) return;
    const LoadedModule* m = module_at(lr);
    if (m && m->info == module_at(find_symbol("nnMain"))->info && lr - m->base == 0x14505fc) {
        static int n = 0;
        uint64_t v = c.x[0];
        if ((v == 0 || ld<uint64_t>(v + 0xa8) == 0) && n++ < 10)
            log("tls: slot %llu = %llx, +0xa8 = %llx (thread %s)", (unsigned long long)slot, (unsigned long long)v,
                (unsigned long long)(v ? ld<uint64_t>(v + 0xa8) : 0), describe(lr).c_str());
    }
}

}  // namespace

NX_HLE("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", GetTlsValue);
NX_HLE("_ZN2nn3mem17StandardAllocator10InitializeEPvm", Initialize);
NX_HLE("_ZN2nn3mem17StandardAllocator10InitializeEPvmb", InitializeCache);
NX_HLE("_ZN2nn3mem17StandardAllocator8AllocateEm", Allocate<kAlloc>);
NX_HLE("_ZN2nn3mem17StandardAllocator8AllocateEmm", Allocate<kAllocAligned>);

}  // namespace nx

// Called from an instrumented copy of the generated code (bring-up only): the engine's heap allocation at
// main+1450630 returned null. Prints the heap object's first words and any name-like strings it points to.
void acnh_heap_fail(nx::CPU& c, uint64_t heap, uint64_t size) {
    using namespace nx;
    if (!nx::alloc_tracing()) return;
    if (c.x[0]) {
        if (size >= 0x40000)
            log("heap: %#llx bytes from heap %#llx (%s) at %llx, caller %s", (unsigned long long)size,
                (unsigned long long)heap, reinterpret_cast<const char*>(ld<uint64_t>(heap + 0x28)), (unsigned long long)c.x[0],
                describe(ld<uint64_t>(c.x[29] + 8)).c_str());
        return;
    }
    log("heap: allocation of %#llx bytes from heap %#llx FAILED", (unsigned long long)size, (unsigned long long)heap);
    for (int k = 0; k < 24; k++) {
        uint64_t v = ld<uint64_t>(heap + 8 * k);
        const char* s = "";
        if (v >= 0x4000000000ull && v < 0x6000000000ull) {
            const char* p = reinterpret_cast<const char*>(v);
            bool text = true;
            for (int i = 0; i < 4; i++) text = text && p[i] >= 0x20 && p[i] < 0x7f;
            if (text) s = p;
        }
        log("  +%#04x %016llx %.40s", k * 8, (unsigned long long)v, s);
    }
}

// Instrumented generated code (bring-up only): a heap-related call site. x2 = the parent heap.
void acnh_probe(nx::CPU& c, const char* tag) {
    using namespace nx;
    if (!alloc_tracing()) return;
    auto name = [](uint64_t h) { return h ? reinterpret_cast<const char*>(ld<uint64_t>(h + 0x28)) : "-"; };
    uint64_t h = c.x[2];
    log("probe %s: x0 %llx x2 %llx (%s) +0x30 %llx +0x38 %llx", tag, (unsigned long long)c.x[0],
        (unsigned long long)h, name(h), (unsigned long long)ld<uint64_t>(h + 0x30), (unsigned long long)ld<uint64_t>(h + 0x38));
    for (uint64_t p = ld<uint64_t>(h + 0x30), k = 0; p && k < 8; p = ld<uint64_t>(p + 0x30), k++)
        log("  parent %llx (%s) +0x38 %llx", (unsigned long long)p, name(p), (unsigned long long)ld<uint64_t>(p + 0x38));
}

// Instrumented entry of sead::ExpHeap::tryAlloc (main+37c7790; this, size, alignment): large allocations from
// the cSys heap, with the caller.
void acnh_alloc_probe(nx::CPU& c) {
    using namespace nx;
    if (!alloc_tracing()) return;
    uint64_t h = c.x[0], size = c.x[1];
    if (size < 0x1000) return;
    const char* name = reinterpret_cast<const char*>(ld<uint64_t>(h + 0x28));
    if (std::strcmp(name, "cSys") == 0) {
        if (size < 0x100000) return;
        log("cSys: %#llx bytes, caller %s", (unsigned long long)size, describe(c.x[30]).c_str());
        return;
    }
    if (size >= 0x40000000) return;  // a child heap taking "the rest"
    if (std::strcmp(name, "TaskHeap") && std::strcmp(name, "AppPreSysHeap") && std::strcmp(name, "FWGfxSystem")) return;
    uint64_t fp = c.x[29];
    log("alloc %s %llx: %#llx bytes, caller %s < %s < %s", name, (unsigned long long)h, (unsigned long long)size,
        describe(c.x[30]).c_str(), describe(ld<uint64_t>(fp + 8)).c_str(), describe(ld<uint64_t>(ld<uint64_t>(fp) + 8)).c_str());
}

// Instrumented entry of sead::ExpHeap::create (main+37c6d38; size, const SafeString& name, parent, ...).
void acnh_create_probe(nx::CPU& c) {
    using namespace nx;
    if (!alloc_tracing()) return;
    uint64_t parent = c.x[2];
    const char* pname = parent ? reinterpret_cast<const char*>(ld<uint64_t>(parent + 0x28)) : "(current)";
    log("heap create '%s' size %#llx in '%s' (caller %s)", reinterpret_cast<const char*>(ld<uint64_t>(c.x[1] + 8)),
        (unsigned long long)c.x[0], pname, describe(c.x[30]).c_str());
}

// Instrumented entry of the engine's "clear on bind" (main+37fb390; this, ?, index, frame context): the per-buffer
// "cleared this frame" word it sets atomically (bit 1), to see why clears repeat (NX_CLEAR_TRACE=<count>).
void acnh_clear_probe(nx::CPU& c) {
    using namespace nx;
    static const char* e = std::getenv("NX_CLEAR_TRACE");
    static std::atomic<int> left{e ? std::atoi(e) : 0};
    if (left <= 0 || g_frame < 600 || left-- <= 0) return;
    uint64_t self = c.x[0], ctx = c.x[3];
    int32_t idx = int32_t(ld<uint32_t>(ctx + 0x58)) - int32_t(ld<uint16_t>(self + 0x3da));
    uint32_t count = ld<uint32_t>(self + 0x368);
    int32_t use = uint32_t(idx) < count ? idx : 0;
    uint64_t entry = ld<uint64_t>(self + 0x370) + 32ull * uint64_t(int64_t(use));
    log("clearprobe: this %llx flags %#x ctx+58 %u base %u count %u -> entry %d word %#x (frame %llu)",
        (unsigned long long)self, ld<uint32_t>(self + 0x5c8), ld<uint32_t>(ctx + 0x58), ld<uint16_t>(self + 0x3da), count,
        use, ld<uint32_t>(entry + 4), (unsigned long long)g_frame.load());
}
