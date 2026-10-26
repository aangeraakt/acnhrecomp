// The setup as a child process, and starting the game (launcher.h).
//
// tools/setup_acnh.py runs in its own process group with its output (stdout and
// stderr) on a pipe: a reader thread splits it into lines, keeps them for the log view, writes them to
// <install>/logs/launcher.log and follows the stage lines ("== <stage>") and ninja's "[n/total]" counter. Cancel
// sends SIGTERM to the whole group (bash, python, ninja and its compilers), then SIGKILL if something is still there
// 5 s later. On Windows the group is a job object, and Cancel terminates it.
#include "launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace launcher {

namespace {

using clock = std::chrono::steady_clock;

std::filesystem::path utf8_path(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }

std::mutex g_lock;
std::vector<std::string> g_lines;
SetupState g_state;
clock::time_point g_t0, g_build_t0;
std::string g_log_path;

// Share of each stage in the progress bar: the first build's compile is most of the time.
const float kWeight[kStageCount] = {0.06f, 0.08f, 0.01f, 0.12f, 0.04f, 0.01f, 0.68f};

int stage_of(const std::string& name) {
    static const std::pair<const char*, int> names[] = {
        {"tools", kStageTools},     {"game", kStageGame},   {"mod ", kStageMods},  {"recompile", kStageRecompile},
        {"shaders", kStageShaders}, {"fonts", kStageFonts},     {"build", kStageBuild}, {"done", kStageCount},
    };
    for (const auto& [n, s] : names)
        if (name.rfind(n, 0) == 0) return s;
    return -1;
}

// Under g_lock.
void on_line(const std::string& line) {
    if (line.rfind("== ", 0) == 0) {
        int s = stage_of(line.substr(3));
        if (s >= 0) {
            for (int k = 0; k < s && k < kStageCount; k++) g_state.done[k] = true;
            g_state.stage = s < kStageCount ? s : g_state.stage;
            if (s == kStageBuild) g_build_t0 = clock::now();
        }
    } else if (g_state.stage == kStageBuild && line.size() > 3 && line[0] == '[') {
        unsigned n = 0, total = 0;
        if (std::sscanf(line.c_str(), "[%u/%u]", &n, &total) == 2 && total) g_state.build_fraction = float(n) / float(total);
    }
    g_state.last_line = line;
}

#if !defined(_WIN32)
pid_t g_pid = -1;
std::atomic<bool> g_cancel{false};

void reader(int fd, FILE* log) {
    std::string pending;
    char buf[8192];
    auto flush = [&] {
        if (pending.empty()) return;
        if (log) std::fprintf(log, "%s\n", pending.c_str()), std::fflush(log);
        std::lock_guard lk(g_lock);
        if (g_lines.size() < 200000) g_lines.push_back(pending);
        on_line(pending);
        pending.clear();
    };
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r')
                flush();
            else if (pending.size() < 4096)
                pending += c;
        }
    }
    flush();
    close(fd);
    int status = 0;
    while (waitpid(g_pid, &status, 0) < 0 && errno == EINTR) {
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    std::lock_guard lk(g_lock);
    g_state.exit_code = code;
    g_state.running = false;
    g_state.finished = true;
    g_state.cancelled = g_cancel;
    g_state.elapsed = std::chrono::duration<double>(clock::now() - g_t0).count();
    if (code == 0)
        for (bool& d : g_state.done) d = true;
    if (log) std::fprintf(log, "\n[launcher] exit code %d\n", code), std::fclose(log);
    g_pid = -1;
}
#endif

}  // namespace

#if defined(_WIN32)
HANDLE g_job = nullptr, g_proc = nullptr;
std::atomic<bool> g_cancel{false};

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// One argument of a command line as CommandLineToArgvW (and Python) read it back.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            slashes++;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}

// A Python 3 to run the setup with: NX_LAUNCHER_PYTHON, the repository's venv, the py launcher, python on PATH.
std::vector<std::wstring> find_python(const std::string& root) {
    if (const char* p = std::getenv("NX_LAUNCHER_PYTHON"); p && *p) return {wide(p)};
    std::error_code ec;
    std::string venv = root + "/private/venv/Scripts/python.exe";
    if (std::filesystem::exists(utf8_path(venv), ec)) return {wide(venv)};
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, found, nullptr)) return {found, L"-3"};
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, found, nullptr)) return {found};
    return {};
}

void reader(HANDLE pipe, FILE* log) {
    std::string pending;
    char buf[8192];
    auto flush = [&] {
        if (pending.empty()) return;
        if (log) std::fprintf(log, "%s\n", pending.c_str()), std::fflush(log);
        std::lock_guard lk(g_lock);
        if (g_lines.size() < 200000) g_lines.push_back(pending);
        on_line(pending);
        pending.clear();
    };
    DWORD n = 0;
    while (ReadFile(pipe, buf, sizeof buf, &n, nullptr) && n) {
        for (DWORD i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r')
                flush();
            else if (pending.size() < 4096)
                pending += c;
        }
    }
    flush();
    CloseHandle(pipe);
    WaitForSingleObject(g_proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(g_proc, &code);
    std::lock_guard lk(g_lock);
    g_state.exit_code = int(code);
    g_state.running = false;
    g_state.finished = true;
    g_state.cancelled = g_cancel;
    g_state.elapsed = std::chrono::duration<double>(clock::now() - g_t0).count();
    if (code == 0)
        for (bool& d : g_state.done) d = true;
    if (log) std::fprintf(log, "\n[launcher] exit code %lu\n", code), std::fclose(log);
    CloseHandle(g_proc);
    CloseHandle(g_job);
    g_proc = g_job = nullptr;
}
#endif

bool setup_supported() { return true; }

bool setup_start(const SetupArgs& a, std::string& error) {
#if defined(_WIN32)
    {
        std::lock_guard lk(g_lock);
        if (g_state.running) return error = "already running", false;
    }
    std::string root = repo_root();
    std::string script = std::getenv("NX_LAUNCHER_SCRIPT") ? std::getenv("NX_LAUNCHER_SCRIPT") : "";  // tests
    if (script.empty()) {
        if (root.empty()) return error = "tools/setup_acnh.py not found", false;
        script = root + "/tools/setup_acnh.py";
    }
    std::vector<std::wstring> args = find_python(root);
    if (args.empty()) return error = "Python 3 not found (winget install Python.Python.3.13)", false;
    args.push_back(L"-u");
    args.push_back(wide(script));
    args.push_back(L"--out");
    args.push_back(wide(a.install_dir));
    auto opt = [&](const wchar_t* name, const std::string& v) {
        if (!v.empty()) args.push_back(name), args.push_back(wide(v));
    };
    opt(L"--game", a.game);
    if (!a.ptbr_zip.empty()) opt(L"--mod", "ptbr=" + a.ptbr_zip);
    std::wstring cmd;
    for (const std::wstring& s : args) cmd += (cmd.empty() ? L"" : L" ") + quote(s);

    std::error_code ec;
    std::filesystem::create_directories(utf8_path(a.install_dir + "/logs"), ec);
    g_log_path = a.install_dir + "/logs/launcher.log";
    FILE* log = _wfopen(utf8_path(g_log_path).c_str(), L"w");
    if (log) {
        int n = WideCharToMultiByte(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string u(size_t(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, cmd.c_str(), -1, u.data(), n, nullptr, nullptr);
        std::fprintf(log, "[launcher] %s\n", u.c_str());
    }

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return error = "CreatePipe failed", false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    std::wstring wcwd = root.empty() ? std::wstring() : wide(root);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        if (log) std::fclose(log);
        return error = "could not start the setup (" + std::to_string(GetLastError()) + ")", false;
    }
    // The setup and everything it starts (pip, the recompiler, ninja and the compilers) live in one job object: Cancel
    // ends them all, and so does the launcher closing.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard lk(g_lock);
        g_lines.clear();
        g_state = SetupState();
        g_state.running = true;
        g_t0 = clock::now();
        g_proc = pi.hProcess;
        g_job = job;
        g_cancel = false;
    }
    std::thread(reader, rd, log).detach();
    return true;
#else
    {
        std::lock_guard lk(g_lock);
        if (g_state.running) return error = "already running", false;
    }
    std::string root = repo_root();
    std::string script = std::getenv("NX_LAUNCHER_SCRIPT") ? std::getenv("NX_LAUNCHER_SCRIPT") : "";  // tests
    if (script.empty()) {
        if (root.empty()) return error = "tools/setup_acnh.py not found", false;
        script = root + "/tools/setup_acnh.py";
    }
    // The setup is Python on every OS: the system's python3 (it makes the private venv it needs).
    std::vector<std::string> args = {script.size() > 3 && script.compare(script.size() - 3, 3, ".sh") == 0 ? "/bin/bash"
                                                                                                          : "/usr/bin/env",
                                     script, "--out", a.install_dir};
    if (args[0] == "/usr/bin/env") args.insert(args.begin() + 1, {"python3", "-u"});
    auto opt = [&](const char* name, const std::string& v) {
        if (!v.empty()) args.push_back(name), args.push_back(v);
    };
    opt("--game", a.game);
    if (!a.ptbr_zip.empty()) opt("--mod", "ptbr=" + a.ptbr_zip);

    std::error_code ec;
    std::filesystem::create_directories(utf8_path(a.install_dir + "/logs"), ec);
    g_log_path = a.install_dir + "/logs/launcher.log";
    FILE* log = std::fopen(g_log_path.c_str(), "w");
    if (log) {
        std::fprintf(log, "[launcher]");
        for (const std::string& s : args) std::fprintf(log, " %s", s.c_str());
        std::fprintf(log, "\n");
    }

    // Everything the child needs is made before fork: only async-signal-safe calls happen between fork and exec.
    std::vector<std::string> envs;
    for (char** e = environ; *e; e++)
        if (std::strncmp(*e, "PYTHONUNBUFFERED=", 17) != 0) envs.push_back(*e);
    envs.push_back("PYTHONUNBUFFERED=1");
    std::vector<char*> argv, envp;
    for (std::string& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    for (std::string& s : envs) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::string cwd = root.empty() ? std::string("/") : root;

    int fds[2];
    if (pipe(fds) != 0) return error = std::strerror(errno), false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]), close(fds[1]);
        return error = std::strerror(errno), false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        if (chdir(cwd.c_str()) != 0) _exit(126);
        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    setpgid(pid, pid);  // also from here: whichever runs first
    close(fds[1]);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    {
        std::lock_guard lk(g_lock);
        g_lines.clear();
        g_state = SetupState();
        g_state.running = true;
        g_t0 = clock::now();
        g_pid = pid;
        g_cancel = false;
    }
    std::thread(reader, fds[0], log).detach();
    return true;
#endif
}

void setup_cancel() {
#if defined(_WIN32)
    std::lock_guard lk(g_lock);
    if (!g_state.running || !g_job) return;
    g_cancel = true;
    TerminateJobObject(g_job, 1);
#else
    pid_t pid;
    {
        std::lock_guard lk(g_lock);
        if (!g_state.running || g_pid <= 0) return;
        pid = g_pid;
    }
    g_cancel = true;
    kill(-pid, SIGTERM);
    std::thread([pid] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::lock_guard lk(g_lock);
        if (g_state.running && g_pid == pid) kill(-pid, SIGKILL);
    }).detach();
#endif
}

SetupState setup_state() {
    std::lock_guard lk(g_lock);
    SetupState s = g_state;
    if (s.running) {
        auto now = clock::now();
        s.elapsed = std::chrono::duration<double>(now - g_t0).count();
        double tb = std::chrono::duration<double>(now - g_build_t0).count();
        if (s.stage == kStageBuild && s.build_fraction > 0.03f && tb > 30)
            s.eta = tb * (1.0 - s.build_fraction) / s.build_fraction;
    }
    return s;
}

float setup_progress(const SetupState& s) {
    float p = 0;
    for (int k = 0; k < kStageCount; k++)
        if (s.done[k]) p += kWeight[k];
    if (s.stage == kStageBuild && !s.done[kStageBuild]) p += kWeight[kStageBuild] * s.build_fraction;
    return std::min(p, 1.0f);
}

void setup_lines(size_t from, std::vector<std::string>& out) {
    std::lock_guard lk(g_lock);
    for (size_t i = from; i < g_lines.size(); i++) out.push_back(g_lines[i]);
}

std::string setup_log_path() { return g_log_path; }

// ---- the game -------------------------------------------------------------------------------------------------

namespace {
SDL_Process* g_game = nullptr;  // the game launch_game started, until game_window_shown() is true
}  // namespace

bool launch_game(const Build& b, std::string& error, SDL_Window* from) {
    SDL_Environment* env = SDL_CreateEnvironment(true);
    if (!env) return error = SDL_GetError(), false;
    if (from) {  // NX_WINDOW_RECT (platform_sdl.cpp): the game's window takes the launcher's place
        int x = 0, y = 0, w = 0, h = 0;
        SDL_GetWindowPosition(from, &x, &y);
        SDL_GetWindowSize(from, &w, &h);
        char rect[96];
        std::snprintf(rect, sizeof rect, "%d,%d,%d,%d%s", x, y, w, h,
                      SDL_GetWindowFlags(from) & SDL_WINDOW_MAXIMIZED ? ",max" : "");
        SDL_SetEnvironmentVariable(env, "NX_WINDOW_RECT", rect, true);
    }
    SDL_SetEnvironmentVariable(env, "NX_DATA_ROOT", b.data.c_str(), true);
    SDL_SetEnvironmentVariable(env, "NX_LOG", (b.data + "/nx_play.log").c_str(), false);
#if defined(__linux__)
    SDL_SetEnvironmentVariable(env, "NX_MEM_LIMIT_GB", "10", false);
#endif
    const char* argv[] = {b.exe.c_str(), nullptr};
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(argv));
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
#if defined(SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING)
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, b.data.c_str());
#endif
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* proc = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    SDL_DestroyEnvironment(env);
    if (!proc) return error = SDL_GetError(), false;
    if (g_game) SDL_DestroyProcess(g_game);
    g_game = proc;  // destroying a background process leaves it running
    return true;
}

#if defined(_WIN32)
namespace {
BOOL CALLBACK find_visible_window(HWND w, LPARAM pid) {
    DWORD owner = 0;
    GetWindowThreadProcessId(w, &owner);
    if (owner != DWORD(pid) || !IsWindowVisible(w)) return TRUE;
    SetLastError(ERROR_SUCCESS);
    return FALSE;  // found: stop
}
}  // namespace
#endif

bool game_window_shown() {
    if (!g_game) return true;
    int code = 0;
    bool shown = SDL_WaitProcess(g_game, false, &code);  // exited
#if defined(_WIN32)
    if (!shown) {
        auto pid = SDL_GetNumberProperty(SDL_GetProcessProperties(g_game), SDL_PROP_PROCESS_PID_NUMBER, 0);
        shown = !EnumWindows(find_visible_window, LPARAM(pid)) && GetLastError() == ERROR_SUCCESS;
    }
#else
    // No portable way to see another process's windows: the caller's timeout decides.
#endif
    if (shown) SDL_DestroyProcess(g_game), g_game = nullptr;
    return shown;
}

namespace {
SDL_AudioStream* g_start_sound = nullptr;
}  // namespace

void start_sound_play() {
    const char* base = SDL_GetBasePath();
    if (!base || g_start_sound) return;
    std::string path = std::string(base) + "start.wav";
    if (!is_file(path) || !SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec spec;
    Uint8* data = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.c_str(), &spec, &data, &len)) return;
    g_start_sound = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_start_sound) {
        SDL_PutAudioStreamData(g_start_sound, data, int(len));
        SDL_FlushAudioStream(g_start_sound);
        SDL_ResumeAudioStreamDevice(g_start_sound);
    }
    SDL_free(data);
}

bool start_sound_playing() { return g_start_sound && SDL_GetAudioStreamQueued(g_start_sound) > 0; }

void open_folder(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(utf8_path(path), ec);
    SDL_OpenURL(("file://" + path).c_str());
}

}  // namespace launcher
