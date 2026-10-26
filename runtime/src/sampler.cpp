// NX_SAMPLE=<hz> (default 997): a SIGPROF sampling profiler for hunting CPU cost where `perf` is not allowed
// (kernel.perf_event_paranoid). Every sample is the interrupted thread and its program counter; every 20 s the top
// buckets per thread are logged as `module+0xoffset` (resolve with `addr2line -f -e <module> 0x...`).
// Linux x86-64: SIGPROF and glibc backtrace. Windows: a thread that suspends the sampled threads in turn and unwinds
// their stacks (NX_SAMPLE_THREADS=<name>,...: the threads whose name contains one, default "replay": the thread that replays
// and presents); the report
// names each function by module and start (acnh.exe+0x...: tools/symbolize.py with acnh.map), both where the thread
// was (self) and the first function of acnh.exe on its stack (ours: where time in the driver comes from).
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <nx/runtime.h>

namespace {

constexpr size_t kRing = 1 << 18;
constexpr int kDepth = 14;
struct Sample { uint32_t tid; uint64_t pc; uint64_t bt[kDepth]; int n; };
Sample g_ring[kRing];
std::atomic<uint64_t> g_head{0};

void handler(int, siginfo_t*, void* uc) {
    auto* c = static_cast<ucontext_t*>(uc);
    uint64_t k = g_head.fetch_add(1, std::memory_order_relaxed);
    Sample& sm = g_ring[k % kRing];
    sm.tid = uint32_t(syscall(SYS_gettid));
    sm.pc = uint64_t(c->uc_mcontext.gregs[REG_RIP]);
    void* frames[kDepth + 3];
    int n = backtrace(frames, kDepth + 3);  // the handler, the trampoline, then the interrupted code
    n = n > 2 ? n - 2 : 0;
    for (int i = 0; i < n && i < kDepth; i++) sm.bt[i] = uint64_t(frames[i + 2]);
    sm.n = n < kDepth ? n : kDepth;
}

std::string comm(uint32_t tid) {
    char p[64], b[64] = "?";
    std::snprintf(p, sizeof p, "/proc/self/task/%u/comm", tid);
    if (FILE* f = std::fopen(p, "r")) {
        if (std::fgets(b, sizeof b, f)) b[std::strcspn(b, "\n")] = 0;
        std::fclose(f);
    }
    return b;
}

void dump_stacks(uint64_t from, uint64_t to) {
    // NX_SAMPLE_STACKS=<file>: raw samples (tid, comm, module+offset per frame) appended for offline aggregation.
    const char* path = std::getenv("NX_SAMPLE_STACKS");
    if (!path) return;
    FILE* f = std::fopen(path, "a");
    if (!f) return;
    std::fprintf(f, "WINDOW\n");
    for (uint64_t k = from; k < to; k++) {
        const Sample& s = g_ring[k % kRing];
        std::fprintf(f, "S %u %s", s.tid, comm(s.tid).c_str());
        uint64_t a = s.pc;
        for (int i = -1; i < s.n; i++) {
            uint64_t pc = i < 0 ? s.pc : s.bt[i];
            Dl_info di{};
            if (dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_fname)
                std::fprintf(f, " %s+%llx", (std::strrchr(di.dli_fname, '/') ? std::strrchr(di.dli_fname, '/') + 1 : di.dli_fname), (unsigned long long)(pc - uint64_t(di.dli_fbase)));
            else std::fprintf(f, " ?+%llx", (unsigned long long)pc);
        }
        (void)a;
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

void report(uint64_t from, uint64_t to) {
    dump_stacks(from, to);
    std::map<uint32_t, uint64_t> per_thread;
    std::map<std::pair<uint32_t, uint64_t>, uint64_t> buckets;
    std::map<std::pair<uint32_t, std::string>, uint64_t> modules;
    for (uint64_t k = from; k < to; k++) {
        const Sample& s = g_ring[k % kRing];
        per_thread[s.tid]++;
        buckets[{s.tid, s.pc >> 4}]++;
        Dl_info di{};
        std::string m = dladdr(reinterpret_cast<void*>(s.pc), &di) && di.dli_fname ? di.dli_fname : "?";
        modules[{s.tid, m.substr(m.rfind('/') + 1)}]++;
    }
    nx::log("sample: %llu samples in the window", (unsigned long long)(to - from));
    for (auto& [tid, n] : per_thread) {
        if (n < 20) continue;
        nx::log("sample: thread %u (%s): %llu samples", tid, comm(tid).c_str(), (unsigned long long)n);
        for (auto& [k, v] : modules) if (k.first == tid && v * 50 >= n) nx::log("sample:   module %s: %llu (%.0f%%)", k.second.c_str(), (unsigned long long)v, 100.0 * v / n);
        std::vector<std::pair<uint64_t, uint64_t>> top;
        for (auto& [k, v] : buckets) if (k.first == tid) top.push_back({v, k.second << 4});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 400; i++) {
            Dl_info di{};
            const void* a = reinterpret_cast<void*>(top[i].second);
            if (dladdr(a, &di) && di.dli_fname) {
                const char* nm = di.dli_sname ? di.dli_sname : "";
                nx::log("sample:   %5.1f%% %s+0x%llx (%s)", 100.0 * top[i].first / n, di.dli_fname,
                        (unsigned long long)(top[i].second - uint64_t(di.dli_fbase)), nm);
            } else nx::log("sample:   %5.1f%% ?0x%llx", 100.0 * top[i].first / n, (unsigned long long)top[i].second);
        }
    }
}

struct Init {
    Init() {
        const char* e = std::getenv("NX_SAMPLE");
        if (!e) return;
        int hz = std::atoi(e);
        if (hz <= 1) hz = 997;
        { void* warm[4]; backtrace(warm, 4); }
        struct sigaction sa{};
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, nullptr);
        itimerval it{{0, 1000000 / hz}, {0, 1000000 / hz}};
        setitimer(ITIMER_PROF, &it, nullptr);
        std::thread([] {
            uint64_t from = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(20));
                uint64_t to = g_head.load();
                if (to - from > kRing) from = to - kRing;
                report(from, to);
                from = to;
            }
        }).detach();
    }
} g_init;

}  // namespace

#elif defined(_WIN32) && defined(_M_X64)
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

namespace {

constexpr size_t kRing = 1 << 18;
constexpr int kDepth = 12;
struct Sample { uint32_t tid; int n; uint64_t pc[kDepth]; };
Sample* g_ring = nullptr;
std::atomic<uint64_t> g_head{0};

struct Target { DWORD tid; HANDLE h; std::string name; };

std::string thread_name(HANDLE h) {
    PWSTR d = nullptr;
    std::string s;
    if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
        char b[128];
        int n = WideCharToMultiByte(CP_UTF8, 0, d, -1, b, sizeof b, nullptr, nullptr);
        if (n > 0) s = b;
        LocalFree(d);
    }
    return s;
}

// The thread's stack while it is suspended: no allocation here (the thread may hold the heap's lock), and a bad
// frame ends the walk instead of the process.
int walk(HANDLE h, uint64_t* out) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(h, &c)) return 0;
    int n = 0;
    __try {
        out[n++] = c.Rip;
        while (n < kDepth) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!f) {  // a leaf: the return address is on top of the stack
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                c.Rsp += 8;
            } else {
                void* data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
            }
            if (!c.Rip) break;
            out[n++] = c.Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void sample(const Target& t) {
    if (SuspendThread(t.h) == DWORD(-1)) return;
    uint64_t pcs[kDepth];
    int n = walk(t.h, pcs);
    ResumeThread(t.h);
    if (!n) return;
    Sample& s = g_ring[g_head.fetch_add(1, std::memory_order_relaxed) % kRing];
    s.tid = t.tid, s.n = n;
    std::memcpy(s.pc, pcs, sizeof(uint64_t) * n);
}

std::vector<std::string> filters() {
    const char* e = std::getenv("NX_SAMPLE_THREADS");
    std::string all = e && *e ? e : "replay";
    std::vector<std::string> v;
    for (size_t a = 0; a <= all.size();) {
        size_t b = all.find(',', a);
        if (b == std::string::npos) b = all.size();
        if (b > a) v.push_back(all.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

void refresh(std::vector<Target>& ts) {
    static const std::vector<std::string> want = filters();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        if (std::any_of(ts.begin(), ts.end(), [&](const Target& t) { return t.tid == te.th32ThreadID; })) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        std::string name = thread_name(h);
        // "replay" and "game main" also name the threads frame_stats.cpp sees in those roles (guest threads, "g<id>").
        for (int role = 0; role < 2; role++)
            if (te.th32ThreadID == nx::plat::frame_thread_id(role)) name += role ? " (game main)" : " (replay+present)";
        bool keep = std::any_of(want.begin(), want.end(), [&](const std::string& w) { return name.find(w) != std::string::npos; });
        if (keep) ts.push_back({te.th32ThreadID, h, name});
        else CloseHandle(h);
    }
    CloseHandle(snap);
}

// module+offset of the function holding pc ("acnh.exe+0x1234"), from its unwind data when it has some.
std::string where(uint64_t pc) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(pc), &m) || !m) {
        char b[32];
        std::snprintf(b, sizeof b, "?0x%llx", (unsigned long long)pc);
        return b;
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\') ? std::strrchr(path, '\\') + 1 : path;
    uint64_t at = pc;
    DWORD64 base = 0;
    if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, nullptr)) at = base + f->BeginAddress;
    char b[MAX_PATH + 32];
    std::snprintf(b, sizeof b, "%s+0x%llx", name, (unsigned long long)(at - uint64_t(m)));
    return b;
}

void report(uint64_t from, uint64_t to, const std::vector<Target>& ts) {
    const uint64_t exe = uint64_t(GetModuleHandleA(nullptr));
    auto in_exe = [&](uint64_t pc) {
        HMODULE m = nullptr;
        return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCSTR>(pc), &m) && uint64_t(m) == exe;
    };
    std::map<uint32_t, uint64_t> per_thread;
    std::map<std::pair<uint32_t, std::string>, uint64_t> self, ours, modules;
    for (uint64_t k = from; k < to; k++) {
        const Sample& s = g_ring[k % kRing];
        per_thread[s.tid]++;
        std::string leaf = where(s.pc[0]);
        self[{s.tid, leaf}]++;
        modules[{s.tid, leaf.substr(0, leaf.find('+'))}]++;
        for (int i = 0; i < s.n; i++)
            if (in_exe(s.pc[i])) {
                ours[{s.tid, where(s.pc[i])}]++;
                break;
            }
    }
    nx::log("sample: %llu samples in the window", (unsigned long long)(to - from));
    for (auto& [tid, n] : per_thread) {
        std::string name = "?";
        for (const Target& t : ts) if (t.tid == tid) name = t.name;
        nx::log("sample: thread %u (%s): %llu samples", tid, name.c_str(), (unsigned long long)n);
        for (auto& [k, v] : modules)
            if (k.first == tid && v * 50 >= n) nx::log("sample:   module %s: %.1f%%", k.second.c_str(), 100.0 * v / n);
        for (int pass = 0; pass < 2; pass++) {
            auto& m = pass ? ours : self;
            std::vector<std::pair<uint64_t, std::string>> top;
            for (auto& [k, v] : m) if (k.first == tid) top.push_back({v, k.second});
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 40 && top[i].first * 200 >= n; i++)
                nx::log("sample:   %s %5.1f%% %s", pass ? "ours" : "self", 100.0 * top[i].first / n, top[i].second.c_str());
        }
    }
}

struct Init {
    Init() {
        const char* e = std::getenv("NX_SAMPLE");
        if (!e) return;
        int hz = std::atoi(e);
        if (hz <= 1) hz = 997;
        g_ring = new Sample[kRing];
        std::thread([hz] {
            std::vector<Target> ts;
            auto next_refresh = std::chrono::steady_clock::now(), next_report = next_refresh + std::chrono::seconds(20);
            uint64_t from = 0;
            const auto period = std::chrono::microseconds(1000000 / hz);
            for (auto at = std::chrono::steady_clock::now();; at += period) {
                auto now = std::chrono::steady_clock::now();
                if (now >= next_refresh) refresh(ts), next_refresh = now + std::chrono::seconds(2);
                for (const Target& t : ts) sample(t);
                if (now >= next_report) {
                    uint64_t to = g_head.load();
                    if (to - from > kRing) from = to - kRing;
                    report(from, to, ts);
                    from = to, next_report = now + std::chrono::seconds(20);
                }
                std::this_thread::sleep_until(at + period);
            }
        }).detach();
    }
} g_init;

}  // namespace

#endif  // Linux x86-64, Windows x64
