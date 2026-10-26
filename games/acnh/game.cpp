// Animal Crossing: New Horizons (01006F8002326000): what the shared runtime needs to know about the game.
// Any version builds (games/acnh/versions.txt, docs/VERSIONS.md); the addresses below are 3.0.2's (3.0.3 shares the
// Phive guard's). The hooks only run in a build recompiled with a hook list that has them, and what runs without a
// hook is applied only when main is a known build.
// The game draws through NVN; what we know about it is in docs/ACNH.md.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"acnh", "Animal Crossing: New Horizons", 0x01006F8002326000ull,
                                "io.github.acnhrecomp", "private/acnh",
                                0 /* main.npdm: no system resource */,
                                // guiTouchManager::update (main+31c3b50) moves every touch 15 up: the UI then hits
                                // what the mouse points at.
                                15};

namespace {

// The engine splits its memory into fixed budgets, a table in main's read-only data (3.0.2: main+0x404d658):
// cSys, cGfx, cRes, cSnd, cScene, cOthers (the last one is created with size 0: whatever is left).
// cSys runs out here before the title (AppSysHeap, created last in cSys with "the rest", is ~11 MB and a 3.8 MB
// allocation fails, main+1450630): the engine creates task heaps in cSys with "the rest" too and trims them when
// the task is done, and our threads do not run in the console's order, so a heap is created while another is not
// trimmed yet. 64 MiB move from cOthers (1.6 GB, a remainder) to cSys. The same table, at another address and with
// 1 MB less cSys, makes 2.0.7 fail the same 3.8 MB allocation (main+abbf84). Applied only where a known build's table
// holds its own values, which is also how the runtime tells the builds apart (budget_build, bind_clears).
struct BudgetTable {
    const char* version;
    uint64_t offset;      // in main
    uint64_t values[6];   // cSys, cGfx, cRes, cSnd, cScene, cOthers
    const char* skip_clear_at;   // clear-on-bind sites whose clears are dropped (lr of the clear call), bind_clears
    const char* clear_once_at;   // and those kept once per frame for each target
    uint64_t code_at = 0;        // where the table alone does not tell the build: a word of main's code there
    uint32_t code_word = 0;
};
constexpr BudgetTable kBudgetTables[] = {
    // 3.0.3: 3.0.2's table at 3.0.2's address, its code 0x10 further from main+31c3b50 on (games/acnh/v3_0_3.cpp):
    // told apart by the model animation's fmul s8, s1, s0 (3.0.2: main+38b2b34).
    {"3.0.3", 0x404d658, {0xa8df000, 0x9900000, 0x4a271f8, 0x900000, 0x23d00000, 0x65e00000},
     "37fb50c", "37fb754", 0x38b2b44, 0x1e200828},
    {"3.0.2", 0x404d658, {0xa8df000, 0x9900000, 0x4a271f8, 0x900000, 0x23d00000, 0x65e00000},
     "37fb4fc", "37fb744"},
    {"2.0.7", 0x39906a8, {0xa7df000, 0x9900000, 0x4a271f8, 0x900000, 0x23d00000, 0x65e00000},
     "", "2741e20,2742070"},
    // 1.0.0: the same table with more cSys and less cGfx/cOthers; its clear helpers are 2.0.7's (main+19ca240,
    // main+19ca4a0), found by their code.
    {"1.0.0", 0x29d4de0, {0xc0df000, 0xa000000, 0x42271f8, 0x8100000, 0x22c00000, 0x607b2000},
     "", "19ca3d0,19ca620"},
};
constexpr uint64_t kSysExtra = 64ull << 20;

uint64_t main_base() {
    const LoadedModule* m = module_at(find_symbol("nnMain"));
    return m ? m->base : 0;
}

// The known build whose budget table is in place (read before widen_sys_budget changes it), or null.
const BudgetTable* budget_build() {
    static const BudgetTable* found = [] {
        const LoadedModule* m = module_at(find_symbol("nnMain"));
        if (!m) return static_cast<const BudgetTable*>(nullptr);
        uint64_t base = m->base;
        for (const BudgetTable& b : kBudgetTables) {
            if (b.offset + 8 * 6 > m->size) continue;  // a smaller main (1.0.0 ends before 3.0.2's table)
            bool same = true;
            for (int k = 0; k < 6 && same; k++) same = ld<uint64_t>(base + b.offset + 8 * k) == b.values[k];
            if (same && b.code_at) same = ld<uint32_t>(base + b.code_at) == b.code_word;
            if (same) return &b;
        }
        return static_cast<const BudgetTable*>(nullptr);
    }();
    return found;
}

void widen_sys_budget(const BudgetTable& b) {
    uint64_t t = main_base() + b.offset;
    st<uint64_t>(t, b.values[0] + kSysExtra);
    st<uint64_t>(t + 8 * 5, b.values[5] - kSysExtra);
    log("acnh %s: cSys budget %#llx -> %#llx (from cOthers)", b.version, (unsigned long long)b.values[0],
        (unsigned long long)(b.values[0] + kSysExtra));
}

}  // namespace

// The engine's "clear on bind" (3.0.2: main+37fb390; 2.0.7: colour main+2741c90, depth main+2741ef0) clears a render
// target every time a pass binds it: colour from main+37fb4fc, depth/stencil from main+37fb744 (2.0.7: main+2741e20,
// main+2742070). It keeps a "cleared" bit per target; on the console the bit is reset by state the GPU keeps, here
// every re-bind clears again. Kept every time:
//  - a re-bind of the HDR target wiped the UI captures drawn into it before (a black screen): those colour clears
//    are dropped (NX_SKIP_CLEAR_AT);
//  - a re-bind of the scene's depth wiped the depth pre-pass, and the colour pass (depth EQUAL) drew the back wall
//    over the characters: only the first depth clear of each target in a frame is kept (NX_CLEAR_ONCE_AT,
//    runtime/src/nvn/nvn_cmd.cpp).
// 2.0.7 keeps its colour clears once per frame too: dropped altogether, the character creator's preview target was
// never cleared and the character left a trail as it moved.
// Either variable set by hand (empty: off) overrides these.
void bind_clears(const BudgetTable& b) {
#if defined(_WIN32)
    if (!std::getenv("NX_SKIP_CLEAR_AT")) _putenv_s("NX_SKIP_CLEAR_AT", b.skip_clear_at);
    if (!std::getenv("NX_CLEAR_ONCE_AT")) _putenv_s("NX_CLEAR_ONCE_AT", b.clear_once_at);
#else
    setenv("NX_SKIP_CLEAR_AT", b.skip_clear_at, 0);
    setenv("NX_CLEAR_ONCE_AT", b.clear_once_at, 0);
#endif
}

// The job workers (sead threads made by the factory at main+6c5b20, vtable main+4816770) register with Havok on a
// message (calc_, main+6d6514): if the Phive singleton exists (main+5a05378), they call its system's thread setup
// ([[[g]+8]+8]->vtable[0x20]) and keep the answer at this+0x18c (-1 until then, so the next message retries). The
// main thread publishes the singleton (main+3dfc120, in main+3dfc044) and only then builds the system (g->init,
// main+3dfc2a8: the 32 MB PhiveSystemHeap and Havok's memory router, ~3 ms here) and stores it at g+8. A worker
// that wakes in between loaded g+8 = null and faulted on address 8 (1 run in ~12, while the Nook Inc. scene loads).
// The wakeups come from the job queue (TrySendMessageQueue at main+3e42c08: the main thread and the workers wake
// idle workers); on the console the workers own cores 1 and 2 and a stale wakeup is handled within microseconds, here
// a host thread under load is woken milliseconds late and lands in that window. Hooked on the null check
// (cbz x8 at main+6d659c, x8 = g): a half-built singleton counts as no singleton, as the game's own physics step
// already does (main+3e01a98 tests g+8).
static void phive_half_built(CPU& c) {
    if (c.x[8] && ld<uint64_t>(c.x[8] + 8) == 0) {
        c.x[8] = 0;
        static std::atomic<int> seen{0};
        if (seen++ < 4) log("acnh: a job worker woke while the Phive singleton is half built: its Havok setup waits");
    }
}
NX_HOOK(0x6d659c, phive_half_built);

void game::after_init() {
    const BudgetTable* b = budget_build();
    if (b) widen_sys_budget(*b);
    else log("acnh: memory budget table of this build not known: left alone (docs/VERSIONS.md)");
    if (b) bind_clears(*b);
    else log("acnh: main is not a known build: the clear-on-bind fix is left out (docs/VERSIONS.md)");
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
