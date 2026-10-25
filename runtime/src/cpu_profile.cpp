// A sampling CPU profiler for the recompiled code (NX_CPU_PROFILE=<interval in us>, e.g. 1000). macOS's `sample` takes
// seconds per sample on a binary this size; here a thread sends SIGPROF to the registered threads (the game thread that
// presents, the replay thread) at that interval, the handler stores the interrupted pc in a ring, and every 20 s the
// counts per pc are written to <data root>/cpu_profile_<thread>.txt with the image slide, for tools/cpu_profile.py to
// name the functions (their symbols are local: dladdr cannot).
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <dlfcn.h>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

constexpr int kThreads = 48;
constexpr uint32_t kRing = 1 << 14;

struct Profiled {
    pthread_t thread{};
    std::string name;
    std::atomic<bool> used{false};
    uint64_t ring[kRing] = {};
    std::atomic<uint32_t> head{0};
    uint32_t tail = 0;
    std::unordered_map<uint64_t, uint32_t> counts;
    uint64_t samples = 0;
};
Profiled g_threads[kThreads];
thread_local int t_slot = -1;

uint64_t interval_us() {
    static const uint64_t us = [] {
        const char* e = std::getenv("NX_CPU_PROFILE");
        return e ? uint64_t(std::max(100, std::atoi(e))) : 0;
    }();
    return us;
}

void on_sigprof(int, siginfo_t*, void* ctx) {
    if (t_slot < 0) return;
    auto* uc = static_cast<ucontext_t*>(ctx);
#if defined(__APPLE__)
    const uint64_t pc = uint64_t(uc->uc_mcontext->__ss.__pc);
#elif defined(__aarch64__)
    const uint64_t pc = uint64_t(uc->uc_mcontext.pc);
#else
    const uint64_t pc = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
#endif
    Profiled& p = g_threads[t_slot];
    const uint32_t h = p.head.load(std::memory_order_relaxed);
    p.ring[h % kRing] = pc;
    p.head.store(h + 1, std::memory_order_release);
}

const void* dladdr_main_base() {
    static const void* base = [] {
        Dl_info di{};
        dladdr(reinterpret_cast<void*>(&on_sigprof), &di);
        return di.dli_fbase;
    }();
    return base;
}

uint64_t image_slide() {
#if defined(__APPLE__)
    return uint64_t(_dyld_get_image_vmaddr_slide(0));
#else
    return 0;
#endif
}

void sampler() {
    const auto period = std::chrono::microseconds(interval_us());
    auto next_write = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        std::this_thread::sleep_for(period);
        for (Profiled& p : g_threads) {
            if (!p.used.load(std::memory_order_acquire)) continue;
            pthread_kill(p.thread, SIGPROF);
            const uint32_t h = p.head.load(std::memory_order_acquire);
            for (; p.tail != h; p.tail++) p.counts[p.ring[p.tail % kRing]]++, p.samples++;
        }
        if (std::chrono::steady_clock::now() < next_write) continue;
        next_write += std::chrono::seconds(20);
        for (Profiled& p : g_threads) {
            if (!p.used.load(std::memory_order_acquire) || !p.samples) continue;
            const std::string path = plat::data_root() + "/cpu_profile_" + p.name + ".txt";
            if (FILE* f = plat::fopen_utf8(path, "w")) {
                std::fprintf(f, "slide %llx samples %llu interval_us %llu\n", (unsigned long long)image_slide(),
                             (unsigned long long)p.samples, (unsigned long long)interval_us());
                // Outside this binary (system libraries, MoltenVK): the exported symbol, named here.
                static std::unordered_map<uint64_t, std::string> names;
                for (auto& [pc, n] : p.counts) {
                    auto it = names.find(pc);
                    if (it == names.end()) {
                        std::string name;
                        Dl_info di{};
                        if (dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_fname &&
                            di.dli_fbase != dladdr_main_base()) {
                            const char* lib = std::strrchr(di.dli_fname, '/');
                            name = std::string(lib ? lib + 1 : di.dli_fname) + ":" + (di.dli_sname ? di.dli_sname : "?");
                        }
                        it = names.emplace(pc, name).first;
                    }
                    std::fprintf(f, "%llx %u %s\n", (unsigned long long)pc, n, it->second.c_str());
                }
                std::fclose(f);
            }
        }
    }
}

}  // namespace

// Called on a thread to be sampled, once; a no-op without NX_CPU_PROFILE.
void cpu_profile_register(const char* name) {
    if (!interval_us() || t_slot >= 0) return;
    static std::atomic<int> next{0};
    static const bool started = [] {
        struct sigaction sa {};
        sa.sa_sigaction = on_sigprof;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        std::thread(sampler).detach();
        log("cpu profile: SIGPROF every %llu us (NX_CPU_PROFILE)", (unsigned long long)interval_us());
        return true;
    }();
    (void)started;
    const int slot = next.fetch_add(1);
    if (slot >= kThreads) return;
    g_threads[slot].thread = pthread_self();
    g_threads[slot].name = name;
    t_slot = slot;
    g_threads[slot].used.store(true, std::memory_order_release);
    log("cpu profile: thread %s sampled", name);
}

}  // namespace nx
