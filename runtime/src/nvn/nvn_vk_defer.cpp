// The replay's Vulkan recording on a thread of its own (NX_VK_DEFER=1; off by default, see enabled()). The replay
// thread decides every draw (state, uploads, sets, pipelines) and used to also make the vkCmd* and
// vkUpdateDescriptorSets calls, which on MoltenVK build its command objects and argument buffers: ~4-5 ms of a 60 fps frame's ~14 ms on a hot M1 Air
// (NX_DRAW_TIMING). While a submit is replayed (VkDeferScope), those calls made on the replay thread are copied, with
// what their pointers point to, into a single-producer ring that the "vk recorder" thread executes in order; the
// replay goes on to the next draw. Calls that end, submit or reset command buffers, wait for the device or destroy
// objects first wait for the ring to empty (vk_defer_drain), so Vulkan sees the same calls in the same order from one
// thread at a time. A call whose structures this file cannot copy (a pNext chain) drains and runs in place.
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <tuple>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#include "nvn_vk.h"
#include "platform.h"

namespace nx {
void cpu_profile_register(const char* name);  // cpu_profile.cpp
}

namespace nx::nvn {

using namespace nx::vk;

namespace {

constexpr uint64_t kRingBytes = 64ull << 20;
uint8_t* g_ring = nullptr;
std::atomic<uint64_t> g_head{0}, g_tail{0};  // bytes written by the replay thread / executed by the recorder
std::atomic<bool> g_sleeping{false};
std::mutex g_lock;
std::condition_variable g_cv;
thread_local bool t_defer = false;  // this thread is replaying inside a VkDeferScope
uint64_t g_deferred_calls = 0, g_fallbacks = 0, g_drains = 0;

using Exec = void (*)(const uint8_t* payload);
struct Header {
    Exec exec;      // null: skip to the start of the ring
    uint32_t size;  // header + payload, 16-byte multiple
};

// The real entry points, called by the recorder (or in place).
struct Real {
#define X(name) PFN_vk##name name = nullptr;
    X(CmdPipelineBarrier) X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdCopyImageToBuffer)
    X(CmdCopyBufferToImage) X(CmdCopyImage) X(CmdCopyBuffer) X(CmdBlitImage) X(CmdBindPipeline)
    X(CmdBindDescriptorSets) X(UpdateDescriptorSets) X(CmdSetViewport) X(CmdSetScissor) X(CmdPushConstants)
    X(CmdDraw) X(CmdDrawIndexed) X(CmdDispatch) X(CmdDispatchIndirect) X(CmdWriteTimestamp) X(CmdResetQueryPool)
    X(CmdBeginQuery) X(CmdEndQuery) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBeginRendering)
    X(CmdEndRendering) X(CmdSetStencilWriteMask) X(CmdSetStencilReference) X(CmdSetStencilCompareMask)
    X(CmdSetDepthBias) X(CmdSetBlendConstants) X(CmdClearAttachments) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer)
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(QueueSubmit) X(QueueWaitIdle)
    X(DeviceWaitIdle) X(QueuePresentKHR) X(DestroyImageView) X(DestroyImage) X(DestroyBuffer) X(DestroyFramebuffer)
    X(DestroySampler) X(DestroyPipeline) X(FreeMemory) X(DestroyRenderPass)
#undef X
} R;

// ---- the ring ---------------------------------------------------------------------------------------------------

void recorder() {
    plat::set_thread_name("vk recorder");
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // a performance core, as the replay thread
#endif
    cpu_profile_register("vk_recorder");
    uint64_t tail = g_tail.load(std::memory_order_relaxed);
    for (;;) {
        uint64_t head = g_head.load(std::memory_order_acquire);
        if (head == tail) {
            // A short spin (the next call is usually a few hundred ns away), then sleep until notified.
            for (int k = 0; k < 2000 && head == tail; k++) {
                if (k > 200) std::this_thread::yield();
                head = g_head.load(std::memory_order_acquire);
            }
            if (head == tail) {
                std::unique_lock lk(g_lock);
                g_sleeping.store(true, std::memory_order_seq_cst);
                g_cv.wait(lk, [&] { return g_head.load(std::memory_order_seq_cst) != tail; });
                g_sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        while (tail != head) {
            const Header* h = reinterpret_cast<const Header*>(g_ring + tail % kRingBytes);
            if (h->exec) h->exec(reinterpret_cast<const uint8_t*>(h + 1));
            tail += h->size;
            g_tail.store(tail, std::memory_order_release);
        }
        g_cv.notify_all();  // a drain may be waiting
    }
}

void wake() {
    if (g_sleeping.load(std::memory_order_seq_cst)) {
        std::lock_guard lk(g_lock);
        g_cv.notify_all();
    }
}

// Room for `bytes` of payload; the caller fills it and calls publish(). Never straddles the end of the ring.
uint8_t* g_reserved = nullptr;
uint64_t g_reserved_size = 0;
uint8_t* reserve(Exec exec, size_t bytes) {
    const uint64_t size = (sizeof(Header) + bytes + 15) & ~15ull;
    uint64_t head = g_head.load(std::memory_order_relaxed);
    uint64_t at = head % kRingBytes;
    if (at + size > kRingBytes) {  // skip to the start
        const uint64_t skip = kRingBytes - at;
        while (head + skip + size - g_tail.load(std::memory_order_acquire) > kRingBytes) std::this_thread::yield();
        Header* h = reinterpret_cast<Header*>(g_ring + at);
        h->exec = nullptr, h->size = uint32_t(skip);
        head += skip;
        g_head.store(head, std::memory_order_release);
        at = 0;
    }
    while (head + size - g_tail.load(std::memory_order_acquire) > kRingBytes) wake(), std::this_thread::yield();
    Header* h = reinterpret_cast<Header*>(g_ring + at);
    h->exec = exec, h->size = uint32_t(size);
    g_reserved_size = size;
    return g_reserved = reinterpret_cast<uint8_t*>(h + 1);
}

void publish() {
    g_head.store(g_head.load(std::memory_order_relaxed) + g_reserved_size, std::memory_order_seq_cst);
    g_deferred_calls++;
    wake();
}

// A bump allocator over a reserved payload: the call's arguments, then copies of what they point to.
struct Writer {
    uint8_t* p;
    template <typename T>
    T* put(const T& v) {
        T* at = reinterpret_cast<T*>(p);
        std::memcpy(at, &v, sizeof(T));
        p += (sizeof(T) + 7) & ~size_t(7);
        return at;
    }
    template <typename T>
    T* array(const T* v, uint32_t n) {
        if (!v || !n) return nullptr;
        T* at = reinterpret_cast<T*>(p);
        std::memcpy(at, v, sizeof(T) * n);
        p += (sizeof(T) * n + 7) & ~size_t(7);
        return at;
    }
};
template <typename T>
size_t bytes_of(uint32_t n) { return (sizeof(T) * n + 7) & ~size_t(7); }
template <typename T>
size_t bytes_of() { return (sizeof(T) + 7) & ~size_t(7); }

bool deferring() { return t_defer && g_ring; }

// ---- wrappers ---------------------------------------------------------------------------------------------------
// Each: deferred when this thread replays (args copied into the ring), else the real call.

// Calls with only scalar arguments: the real function and its arguments, copied as a tuple and applied.
template <typename F, typename... Args>
void enqueue(F fn, Args... args) {
    using T = std::tuple<F, Args...>;
    Writer w{reserve([](const uint8_t* p) {
        std::apply([](auto f, auto... a) { f(a...); }, *reinterpret_cast<const T*>(p));
    }, sizeof(T))};
    new (w.p) T(fn, args...);
    publish();
}

#define SIMPLE(name, PARAMS, ARGS)                                   \
    VKAPI_ATTR void VKAPI_CALL W_##name PARAMS {                      \
        if (!deferring()) return R.name ARGS;                         \
        enqueue(R.name, ARGS_UNPACK ARGS);                            \
    }
#define ARGS_UNPACK(...) __VA_ARGS__
SIMPLE(CmdBindPipeline, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p), (cb, bp, p))
SIMPLE(CmdDraw, (VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi), (cb, vc, ic, fv, fi))
SIMPLE(CmdDrawIndexed, (VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t fin),
       (cb, ic, inst, fi, vo, fin))
SIMPLE(CmdDispatch, (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z), (cb, x, y, z))
SIMPLE(CmdDispatchIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o), (cb, b, o))
SIMPLE(CmdWriteTimestamp, (VkCommandBuffer cb, VkPipelineStageFlagBits s, VkQueryPool q, uint32_t i), (cb, s, q, i))
SIMPLE(CmdResetQueryPool, (VkCommandBuffer cb, VkQueryPool q, uint32_t f, uint32_t n), (cb, q, f, n))
SIMPLE(CmdBeginQuery, (VkCommandBuffer cb, VkQueryPool q, uint32_t i, VkQueryControlFlags f), (cb, q, i, f))
SIMPLE(CmdEndQuery, (VkCommandBuffer cb, VkQueryPool q, uint32_t i), (cb, q, i))
SIMPLE(CmdEndRenderPass, (VkCommandBuffer cb), (cb))
SIMPLE(CmdEndRendering, (VkCommandBuffer cb), (cb))
SIMPLE(CmdSetStencilWriteMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v))
SIMPLE(CmdSetStencilReference, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v))
SIMPLE(CmdSetStencilCompareMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v))
SIMPLE(CmdSetDepthBias, (VkCommandBuffer cb, float c, float cl, float s), (cb, c, cl, s))
SIMPLE(CmdBindIndexBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t), (cb, b, o, t))
#undef SIMPLE
#undef ARGS_UNPACK

VKAPI_ATTR void VKAPI_CALL W_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (!deferring()) return R.CmdSetBlendConstants(cb, c);
    struct A { VkCommandBuffer cb; float c[4]; } a{cb, {c[0], c[1], c[2], c[3]}};
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdSetBlendConstants(a.cb, a.c);
    }, sizeof a)};
    w.put(a);
    publish();
}

// Calls with arrays: the header struct holds the copies' pointers (into the ring, valid until executed).
VKAPI_ATTR void VKAPI_CALL W_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    if (!deferring()) return R.CmdSetViewport(cb, first, n, v);
    struct A { VkCommandBuffer cb; uint32_t first, n; const VkViewport* v; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdSetViewport(a.cb, a.first, a.n, a.v);
    }, bytes_of<A>() + bytes_of<VkViewport>(n))};
    A* a = w.put(A{cb, first, n, nullptr});
    a->v = w.array(v, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    if (!deferring()) return R.CmdSetScissor(cb, first, n, r);
    struct A { VkCommandBuffer cb; uint32_t first, n; const VkRect2D* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdSetScissor(a.cb, a.first, a.n, a.r);
    }, bytes_of<A>() + bytes_of<VkRect2D>(n))};
    A* a = w.put(A{cb, first, n, nullptr});
    a->r = w.array(r, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags s,
                                              uint32_t off, uint32_t size, const void* v) {
    if (!deferring()) return R.CmdPushConstants(cb, l, s, off, size, v);
    struct A { VkCommandBuffer cb; VkPipelineLayout l; VkShaderStageFlags s; uint32_t off, size; const uint8_t* v; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdPushConstants(a.cb, a.l, a.s, a.off, a.size, a.v);
    }, bytes_of<A>() + bytes_of<uint8_t>(size))};
    A* a = w.put(A{cb, l, s, off, size, nullptr});
    a->v = w.array(static_cast<const uint8_t*>(v), size);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
                                                   uint32_t first, uint32_t n, const VkDescriptorSet* sets,
                                                   uint32_t nd, const uint32_t* dyn) {
    if (!deferring()) return R.CmdBindDescriptorSets(cb, bp, l, first, n, sets, nd, dyn);
    struct A {
        VkCommandBuffer cb; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd;
        const VkDescriptorSet* sets; const uint32_t* dyn;
    };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdBindDescriptorSets(a.cb, a.bp, a.l, a.first, a.n, a.sets, a.nd, a.dyn);
    }, bytes_of<A>() + bytes_of<VkDescriptorSet>(n) + bytes_of<uint32_t>(nd))};
    A* a = w.put(A{cb, bp, l, first, n, nd, nullptr, nullptr});
    a->sets = w.array(sets, n), a->dyn = w.array(dyn, nd);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b,
                                                  const VkDeviceSize* o) {
    if (!deferring()) return R.CmdBindVertexBuffers(cb, first, n, b, o);
    struct A { VkCommandBuffer cb; uint32_t first, n; const VkBuffer* b; const VkDeviceSize* o; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdBindVertexBuffers(a.cb, a.first, a.n, a.b, a.o);
    }, bytes_of<A>() + bytes_of<VkBuffer>(n) + bytes_of<VkDeviceSize>(n))};
    A* a = w.put(A{cb, first, n, nullptr, nullptr});
    a->b = w.array(b, n), a->o = w.array(o, n);
    publish();
}

// Calls that end, submit or reset command buffers, wait, or destroy what earlier calls may use: the ring first.
#define SYNC(ret, name, PARAMS, ARGS)                                   \
    VKAPI_ATTR ret VKAPI_CALL W_##name PARAMS {                         \
        if (t_defer) vk_defer_drain();                                  \
        return R.name ARGS;                                             \
    }
SYNC(VkResult, BeginCommandBuffer, (VkCommandBuffer cb, const VkCommandBufferBeginInfo* i), (cb, i))
SYNC(VkResult, EndCommandBuffer, (VkCommandBuffer cb), (cb))
SYNC(VkResult, ResetCommandBuffer, (VkCommandBuffer cb, VkCommandBufferResetFlags f), (cb, f))
SYNC(VkResult, QueueSubmit, (VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f), (q, n, s, f))
SYNC(VkResult, QueueWaitIdle, (VkQueue q), (q))
SYNC(VkResult, DeviceWaitIdle, (VkDevice d), (d))
SYNC(VkResult, QueuePresentKHR, (VkQueue q, const VkPresentInfoKHR* i), (q, i))
SYNC(void, DestroyImageView, (VkDevice d, VkImageView v, const VkAllocationCallbacks* a), (d, v, a))
SYNC(void, DestroyImage, (VkDevice d, VkImage v, const VkAllocationCallbacks* a), (d, v, a))
SYNC(void, DestroyBuffer, (VkDevice d, VkBuffer v, const VkAllocationCallbacks* a), (d, v, a))
SYNC(void, DestroyFramebuffer, (VkDevice d, VkFramebuffer v, const VkAllocationCallbacks* a), (d, v, a))
SYNC(void, DestroySampler, (VkDevice d, VkSampler v, const VkAllocationCallbacks* a), (d, v, a))
SYNC(void, DestroyPipeline, (VkDevice d, VkPipeline v, const VkAllocationCallbacks* a), (d, v, a))
SYNC(void, DestroyRenderPass, (VkDevice d, VkRenderPass v, const VkAllocationCallbacks* a), (d, v, a))
SYNC(void, FreeMemory, (VkDevice d, VkDeviceMemory v, const VkAllocationCallbacks* a), (d, v, a))
#undef SYNC

// Calls with structures; a pNext anywhere: run in place, after the ring.
#define IN_PLACE(call)                    \
    do {                                  \
        vk_defer_drain();                 \
        g_fallbacks++;                    \
        return call;                      \
    } while (0)

VKAPI_ATTR void VKAPI_CALL W_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                                VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier* m,
                                                uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
                                                const VkImageMemoryBarrier* im) {
    if (!deferring()) return R.CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im);
    for (uint32_t k = 0; k < nm; k++)
        if (m[k].pNext) IN_PLACE(R.CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im));
    for (uint32_t k = 0; k < nb; k++)
        if (b[k].pNext) IN_PLACE(R.CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im));
    for (uint32_t k = 0; k < ni; k++)
        if (im[k].pNext) IN_PLACE(R.CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im));
    struct A {
        VkCommandBuffer cb; VkPipelineStageFlags src, dst; VkDependencyFlags dep; uint32_t nm, nb, ni;
        const VkMemoryBarrier* m; const VkBufferMemoryBarrier* b; const VkImageMemoryBarrier* im;
    };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdPipelineBarrier(a.cb, a.src, a.dst, a.dep, a.nm, a.m, a.nb, a.b, a.ni, a.im);
    }, bytes_of<A>() + bytes_of<VkMemoryBarrier>(nm) + bytes_of<VkBufferMemoryBarrier>(nb) +
                     bytes_of<VkImageMemoryBarrier>(ni))};
    A* a = w.put(A{cb, src, dst, dep, nm, nb, ni, nullptr, nullptr, nullptr});
    a->m = w.array(m, nm), a->b = w.array(b, nb), a->im = w.array(im, ni);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l,
                                                const VkClearColorValue* c, uint32_t n,
                                                const VkImageSubresourceRange* r) {
    if (!deferring()) return R.CmdClearColorImage(cb, img, l, c, n, r);
    struct A { VkCommandBuffer cb; VkImage img; VkImageLayout l; uint32_t n; VkClearColorValue c; const VkImageSubresourceRange* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdClearColorImage(a.cb, a.img, a.l, &a.c, a.n, a.r);
    }, bytes_of<A>() + bytes_of<VkImageSubresourceRange>(n))};
    A* a = w.put(A{cb, img, l, n, *c, nullptr});
    a->r = w.array(r, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l,
                                                       const VkClearDepthStencilValue* c, uint32_t n,
                                                       const VkImageSubresourceRange* r) {
    if (!deferring()) return R.CmdClearDepthStencilImage(cb, img, l, c, n, r);
    struct A { VkCommandBuffer cb; VkImage img; VkImageLayout l; uint32_t n; VkClearDepthStencilValue c; const VkImageSubresourceRange* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdClearDepthStencilImage(a.cb, a.img, a.l, &a.c, a.n, a.r);
    }, bytes_of<A>() + bytes_of<VkImageSubresourceRange>(n))};
    A* a = w.put(A{cb, img, l, n, *c, nullptr});
    a->r = w.array(r, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage img, VkImageLayout l, VkBuffer buf,
                                                  uint32_t n, const VkBufferImageCopy* r) {
    if (!deferring()) return R.CmdCopyImageToBuffer(cb, img, l, buf, n, r);
    struct A { VkCommandBuffer cb; VkImage img; VkImageLayout l; VkBuffer buf; uint32_t n; const VkBufferImageCopy* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdCopyImageToBuffer(a.cb, a.img, a.l, a.buf, a.n, a.r);
    }, bytes_of<A>() + bytes_of<VkBufferImageCopy>(n))};
    A* a = w.put(A{cb, img, l, buf, n, nullptr});
    a->r = w.array(r, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img, VkImageLayout l,
                                                  uint32_t n, const VkBufferImageCopy* r) {
    if (!deferring()) return R.CmdCopyBufferToImage(cb, buf, img, l, n, r);
    struct A { VkCommandBuffer cb; VkBuffer buf; VkImage img; VkImageLayout l; uint32_t n; const VkBufferImageCopy* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdCopyBufferToImage(a.cb, a.buf, a.img, a.l, a.n, a.r);
    }, bytes_of<A>() + bytes_of<VkBufferImageCopy>(n))};
    A* a = w.put(A{cb, buf, img, l, n, nullptr});
    a->r = w.array(r, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                          uint32_t n, const VkImageCopy* r) {
    if (!deferring()) return R.CmdCopyImage(cb, s, sl, d, dl, n, r);
    struct A { VkCommandBuffer cb; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageCopy* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdCopyImage(a.cb, a.s, a.sl, a.d, a.dl, a.n, a.r);
    }, bytes_of<A>() + bytes_of<VkImageCopy>(n))};
    A* a = w.put(A{cb, s, sl, d, dl, n, nullptr});
    a->r = w.array(r, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) {
    if (!deferring()) return R.CmdCopyBuffer(cb, s, d, n, r);
    struct A { VkCommandBuffer cb; VkBuffer s, d; uint32_t n; const VkBufferCopy* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdCopyBuffer(a.cb, a.s, a.d, a.n, a.r);
    }, bytes_of<A>() + bytes_of<VkBufferCopy>(n))};
    A* a = w.put(A{cb, s, d, n, nullptr});
    a->r = w.array(r, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                          uint32_t n, const VkImageBlit* r, VkFilter f) {
    if (!deferring()) return R.CmdBlitImage(cb, s, sl, d, dl, n, r, f);
    struct A { VkCommandBuffer cb; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; VkFilter f; const VkImageBlit* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdBlitImage(a.cb, a.s, a.sl, a.d, a.dl, a.n, a.r, a.f);
    }, bytes_of<A>() + bytes_of<VkImageBlit>(n))};
    A* a = w.put(A{cb, s, sl, d, dl, n, f, nullptr});
    a->r = w.array(r, n);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* at,
                                                 uint32_t nr, const VkClearRect* r) {
    if (!deferring()) return R.CmdClearAttachments(cb, na, at, nr, r);
    struct A { VkCommandBuffer cb; uint32_t na, nr; const VkClearAttachment* at; const VkClearRect* r; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdClearAttachments(a.cb, a.na, a.at, a.nr, a.r);
    }, bytes_of<A>() + bytes_of<VkClearAttachment>(na) + bytes_of<VkClearRect>(nr))};
    A* a = w.put(A{cb, na, nr, nullptr, nullptr});
    a->at = w.array(at, na), a->r = w.array(r, nr);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* i,
                                                VkSubpassContents c) {
    if (!deferring()) return R.CmdBeginRenderPass(cb, i, c);
    if (i->pNext) IN_PLACE(R.CmdBeginRenderPass(cb, i, c));
    struct A { VkCommandBuffer cb; VkSubpassContents c; VkRenderPassBeginInfo i; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdBeginRenderPass(a.cb, &a.i, a.c);
    }, bytes_of<A>() + bytes_of<VkClearValue>(i->clearValueCount))};
    A* a = w.put(A{cb, c, *i});
    a->i.pClearValues = w.array(i->pClearValues, i->clearValueCount);
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* i) {
    if (!deferring()) return R.CmdBeginRendering(cb, i);
    bool chains = i->pNext || (i->pDepthAttachment && i->pDepthAttachment->pNext) ||
                  (i->pStencilAttachment && i->pStencilAttachment->pNext);
    for (uint32_t k = 0; k < i->colorAttachmentCount; k++) chains = chains || i->pColorAttachments[k].pNext;
    if (chains) IN_PLACE(R.CmdBeginRendering(cb, i));
    struct A { VkCommandBuffer cb; VkRenderingInfo i; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.CmdBeginRendering(a.cb, &a.i);
    }, bytes_of<A>() + bytes_of<VkRenderingAttachmentInfo>(i->colorAttachmentCount) +
                     2 * bytes_of<VkRenderingAttachmentInfo>())};
    A* a = w.put(A{cb, *i});
    a->i.pColorAttachments = w.array(i->pColorAttachments, i->colorAttachmentCount);
    a->i.pDepthAttachment = i->pDepthAttachment ? w.put(*i->pDepthAttachment) : nullptr;
    a->i.pStencilAttachment = i->pStencilAttachment ? w.put(*i->pStencilAttachment) : nullptr;
    publish();
}

VKAPI_ATTR void VKAPI_CALL W_UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet* ws, uint32_t nc,
                                                  const VkCopyDescriptorSet* cs) {
    if (!deferring() || nc) {
        if (deferring()) vk_defer_drain();
        return R.UpdateDescriptorSets(d, nw, ws, nc, cs);
    }
    size_t bytes = bytes_of<VkDevice>() + bytes_of<uint32_t>() + bytes_of<VkWriteDescriptorSet>(nw);
    for (uint32_t k = 0; k < nw; k++) {
        if (ws[k].pNext) IN_PLACE(R.UpdateDescriptorSets(d, nw, ws, nc, cs));
        const uint32_t n = ws[k].descriptorCount;
        if (ws[k].pBufferInfo) bytes += bytes_of<VkDescriptorBufferInfo>(n);
        if (ws[k].pImageInfo) bytes += bytes_of<VkDescriptorImageInfo>(n);
        if (ws[k].pTexelBufferView) bytes += bytes_of<VkBufferView>(n);
    }
    struct A { VkDevice d; uint32_t nw; VkWriteDescriptorSet* ws; };
    Writer w{reserve([](const uint8_t* p) {
        const A& a = *reinterpret_cast<const A*>(p);
        R.UpdateDescriptorSets(a.d, a.nw, a.ws, 0, nullptr);
    }, bytes + bytes_of<A>())};
    A* a = w.put(A{d, nw, nullptr});
    a->ws = w.array(ws, nw);
    for (uint32_t k = 0; k < nw; k++) {
        const uint32_t n = ws[k].descriptorCount;
        if (ws[k].pBufferInfo) a->ws[k].pBufferInfo = w.array(ws[k].pBufferInfo, n);
        if (ws[k].pImageInfo) a->ws[k].pImageInfo = w.array(ws[k].pImageInfo, n);
        if (ws[k].pTexelBufferView) a->ws[k].pTexelBufferView = w.array(ws[k].pTexelBufferView, n);
    }
    publish();
}
#undef IN_PLACE

bool enabled() {
    static const bool on = [] {
        // Off by default until it is measured to help: the replay got faster (14 -> 8 ms on a hot M1 Air at 60 fps),
        // but frames then waited ~8 ms on fences and the frame rate did not rise. NX_VK_DEFER=1: on.
        const char* e = std::getenv("NX_VK_DEFER");
        return e && e[0] == '1';
    }();
    return on;
}

}  // namespace

// After the device's functions are loaded: the wrappers take the place of the entry points they cover.
void vk_defer_install() {
    if (!enabled() || g_ring) return;
#define SWAP(name)                         \
    R.name = p_vk##name;                   \
    if (R.name) p_vk##name = W_##name;
    SWAP(CmdPipelineBarrier) SWAP(CmdClearColorImage) SWAP(CmdClearDepthStencilImage) SWAP(CmdCopyImageToBuffer)
    SWAP(CmdCopyBufferToImage) SWAP(CmdCopyImage) SWAP(CmdCopyBuffer) SWAP(CmdBlitImage) SWAP(CmdBindPipeline)
    SWAP(CmdBindDescriptorSets) SWAP(UpdateDescriptorSets) SWAP(CmdSetViewport) SWAP(CmdSetScissor)
    SWAP(CmdPushConstants) SWAP(CmdDraw) SWAP(CmdDrawIndexed) SWAP(CmdDispatch) SWAP(CmdDispatchIndirect)
    SWAP(CmdWriteTimestamp) SWAP(CmdResetQueryPool) SWAP(CmdBeginQuery) SWAP(CmdEndQuery) SWAP(CmdBeginRenderPass)
    SWAP(CmdEndRenderPass) SWAP(CmdBeginRendering) SWAP(CmdEndRendering) SWAP(CmdSetStencilWriteMask)
    SWAP(CmdSetStencilReference) SWAP(CmdSetStencilCompareMask) SWAP(CmdSetDepthBias) SWAP(CmdSetBlendConstants)
    SWAP(CmdClearAttachments) SWAP(CmdBindVertexBuffers) SWAP(CmdBindIndexBuffer) SWAP(BeginCommandBuffer)
    SWAP(EndCommandBuffer) SWAP(ResetCommandBuffer) SWAP(QueueSubmit) SWAP(QueueWaitIdle) SWAP(DeviceWaitIdle)
    SWAP(QueuePresentKHR) SWAP(DestroyImageView) SWAP(DestroyImage) SWAP(DestroyBuffer) SWAP(DestroyFramebuffer)
    SWAP(DestroySampler) SWAP(DestroyPipeline) SWAP(FreeMemory) SWAP(DestroyRenderPass)
#undef SWAP
    g_ring = static_cast<uint8_t*>(std::malloc(kRingBytes));
    std::thread(recorder).detach();
    log("nvn/vk: Vulkan recording on its own thread (NX_VK_DEFER=1)");
}

void vk_defer_drain() {
    if (!g_ring) return;
    const uint64_t head = g_head.load(std::memory_order_seq_cst);
    if (g_tail.load(std::memory_order_acquire) == head) return;
    g_drains++;
    wake();
    for (int k = 0; g_tail.load(std::memory_order_acquire) != head; k++)
        if (k > 1000) {
            std::unique_lock lk(g_lock);
            g_cv.wait_for(lk, std::chrono::microseconds(200),
                          [&] { return g_tail.load(std::memory_order_acquire) == head; });
        }
}

VkDeferScope::VkDeferScope() : on_(g_ring != nullptr) {
    if (on_) t_defer = true;
}
VkDeferScope::~VkDeferScope() {
    if (!on_) return;
    vk_defer_drain();
    t_defer = false;
    static uint64_t scopes = 0;
    if (++scopes % 600 == 0) {
        log("nvn/vk: deferred Vulkan calls: %.0f per submit, %llu run in place, %.1f drains per submit",
            double(g_deferred_calls) / 600.0, (unsigned long long)g_fallbacks, double(g_drains) / 600.0);
        g_deferred_calls = g_fallbacks = g_drains = 0;
    }
}

}  // namespace nx::nvn
