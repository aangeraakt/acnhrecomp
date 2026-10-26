// movie::Decoder (SDK_movie, subsdk0) for VP8/VP9 video and Vorbis audio, decoded on the CPU with libvpx
// and libvorbis.
// First needed by Animal Crossing: New Horizons: the opening movie (Movie/OpeningMovie.webm, VP9 1080p + Vorbis).
// The SDK's decoder is Android's stagefright on the console's NVDEC block (NvMM/OMX): it opens the nvhost driver
// and aborts in process. The game demuxes with movie::Extractor (software, it runs recompiled) and drives the
// decoder like Android's MediaCodec: input buffer indexes, output buffer indexes and four os::Events
// (movie::DecoderEvents: input available, output available, format changed, error).
//
// main creates the decoder with `new movie::Decoder(type, output format, mode, priority)` and calls everything
// else through its vtable, so the constructor is replaced: for video types the object gets a vtable of our own whose
// slots are HLE thunks (the slot order is movie::Decoder's, read from _ZTVN5movie7DecoderE). The SDK's Vorbis
// decoder is software (OMX.google on ALooper threads) but stalled after its first buffers here, and the demuxer
// then starved the video too, so Vorbis is replaced as well. Other types (AVC, AAC) keep the SDK's implementation.
//
// Video output (format 4) is NV12, tightly packed (a width x height luma plane, then interleaved chroma at half
// resolution), copied into the buffer the game passes to GetOutputBuffer, as DecoderImpl::ConvertDecoderOutputFormat
// does. Audio output (format 1) is interleaved s16 PCM. Vorbis headers come as Android's csd-0 (identification)
// and csd-1 (setup) buffers in the track format. Without libvpx (NX_NO_VPX) frames are black; without libvorbis
// (NX_NO_VORBIS) the SDK's own Vorbis decoder is used.
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(NX_NO_VPX)
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#endif
#if !defined(NX_NO_VORBIS)
#include <vorbis/codec.h>
#endif

#include <nx/runtime.h>

namespace nx {
namespace nvn {
uint64_t guest_call(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args);
uint64_t scratch(CPU& c, size_t size);
}  // namespace nvn

namespace {

using nvn::guest_call;

constexpr const char* kCtor = "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi";
constexpr uint32_t kOk = 0, kInvalid = 0x100b, kNoBuffer = 0x1011, kNoOutputBuffer = 0x100e;
constexpr uint32_t kFlagEos = 4;          // BufferFlag_EndOfStream (what main passes when the extractor runs out)
// Buffers in flight. The game's extractor message queue holds 32 messages total across audio + video inputs,
// and the game's video output ring buffer [x19 + 0x150] holds 32 entries without bounds checking on push.
// Keeping kInputs = 8 (8 + 8 = 16 < 32) and kVideoOutputs = 4, kAudioOutputs = 8 prevents any ring buffer overflow.
constexpr int kInputs = 8, kVideoOutputs = 4, kAudioOutputs = 8;
constexpr uint64_t kInputCapacity = 4ull << 20;

enum Type { kAvc = 1, kVp8 = 2, kVp9 = 3, kAac = 4, kVorbis = 5 };

struct Frame {
    std::vector<uint8_t> data;  // NV12 picture or s16 PCM
    int64_t pts = 0;
    uint32_t flags = 0;
    bool acquired = false;
    int index = -1;
};

struct Decoder {
    std::mutex lock;
    int type = 0, out_format = 0;
    int width = 0, height = 0;
    int channels = 0, rate = 0;  // audio
    // movie::DecoderEvents*: {input available, output available, format changed, error} (os::EventType*). Read when
    // signalling, as DecoderImpl does: the game registers the struct before it fills it.
    uint64_t events = 0;
    uint64_t inputs[kInputs] = {};
    std::deque<int> free_inputs, free_outputs;
    std::deque<Frame> decoded;
    std::map<int, Frame> active_outputs;
    Frame current_frame;
    uint64_t play_start_ns = 0, last_copy_ns = 0;
    int64_t first_pts = 0;
    bool started = false, format_sent = false;
    bool eos = false, eos_sent = false;  // the end-of-stream input came / was delivered to the guest
    uint64_t frames = 0;
    uint64_t sent = 0, acquired = 0, copied = 0, released = 0;  // logged every 300 frames copied
    uint64_t last_dst = 0, last_cap = 0;
#if !defined(NX_NO_VPX)
    vpx_codec_ctx_t vpx{};
    bool vpx_ok = false;
#endif
#if !defined(NX_NO_VORBIS)
    vorbis_info vi{};
    vorbis_comment vc{};
    vorbis_dsp_state vd{};
    vorbis_block vb{};
    bool vorbis_ok = false;
    int64_t packetno = 0;
#endif
    bool audio() const { return type == kVorbis; }
    int max_outputs() const { return audio() ? kAudioOutputs : kVideoOutputs; }
};

std::mutex g_lock;
std::map<uint64_t, std::unique_ptr<Decoder>> g_decoders;  // by guest object
uint64_t g_vtable = 0;                                   // guest copy of our vtable (address point)
uint64_t g_strings = 0;                                  // guest copies of the MediaData keys

Decoder* of(uint64_t self) {
    std::lock_guard lk(g_lock);
    auto it = g_decoders.find(self);
    return it == g_decoders.end() ? nullptr : it->second.get();
}

uint64_t plat_now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t sym(const char* name) {
    uint64_t a = find_symbol(name);
    if (!a) fatal("movie: %s not found", name);
    return a;
}

// MediaData keys live in guest memory: the SDK's setters keep the pointer they are given only for the call.
uint64_t key(const char* k) {
    static std::mutex m;
    static std::map<std::string, uint64_t> keys;
    static uint64_t next = 0;
    std::lock_guard lk(m);
    auto it = keys.find(k);
    if (it != keys.end()) return it->second;
    if (!g_strings || next + std::strlen(k) + 1 > 0x1000) g_strings = kernel_alloc(0x1000), next = 0;
    uint64_t a = g_strings + next;
    std::memcpy(reinterpret_cast<void*>(a), k, std::strlen(k) + 1);
    next += (std::strlen(k) + 16) & ~uint64_t(15);
    keys[k] = a;
    return a;
}

// NX_MOVIE_STATS=1: every 2 s, each decoder's buffers.
void stats() {
    static const bool on = std::getenv("NX_MOVIE_STATS") != nullptr;
    static std::atomic<uint64_t> next{0};
    uint64_t now = plat_now_ns();
    if (!on || now < next) return;
    next = now + 2'000'000'000ull;
    std::lock_guard lk(g_lock);
    for (auto& [self, d] : g_decoders) {
        log("movie: %s: %llu sent, %llu decoded, %llu acquired, %llu copied, %llu released; decoded queue %zu, "
            "active %zu, inputs %zu free, %zu output slots free", d->audio() ? "audio" : "video", (unsigned long long)d->sent,
            (unsigned long long)d->frames, (unsigned long long)d->acquired, (unsigned long long)d->copied,
            (unsigned long long)d->released, d->decoded.size(), d->active_outputs.size(), d->free_inputs.size(), d->free_outputs.size());
    }
}

void signal(CPU& c, Decoder& d, int which) {
    stats();
    static const uint64_t fn = sym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    uint64_t ev = d.events ? ld<uint64_t>(d.events + 8 * which) : 0;
    static const bool trace = std::getenv("NX_MOVIE_TRACE") != nullptr;
    if (trace) log("movie: %s signal %d (event %llx)", d.audio() ? "audio" : "video", which, (unsigned long long)ev);
    if (ev) guest_call(c, fn, {ev});
}

// ---- decoding ----------------------------------------------------------------------------------------------------

void open_codec(Decoder& d) {
#if !defined(NX_NO_VPX)
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = 4;
    vpx_codec_iface_t* iface = d.type == kVp8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx();
    d.vpx_ok = vpx_codec_dec_init(&d.vpx, iface, &cfg, 0) == VPX_CODEC_OK;
    log("movie: %s decoder %s (libvpx %s)", d.type == kVp8 ? "VP8" : "VP9", d.vpx_ok ? "ready" : "failed",
        vpx_codec_version_str());
#else
    log("movie: built without libvpx: video frames are black");
#endif
}

// One compressed frame in, zero or more NV12 frames out (appended to d.decoded).
void decode(Decoder& d, const uint8_t* data, size_t size, int64_t pts, uint32_t flags) {
    auto push = [&](std::vector<uint8_t>&& bytes, int64_t frame_pts, uint32_t fl) {
        Frame f;
        f.data = std::move(bytes);
        f.pts = frame_pts;
        f.flags = fl;
        if (!d.audio() && d.current_frame.data.empty() && !f.data.empty()) {
            d.current_frame = f;
        }
        d.decoded.push_back(std::move(f));
    };
#if !defined(NX_NO_VORBIS)
    if (d.audio()) {
        if (d.vorbis_ok && size) {
            ogg_packet op{};
            op.packet = const_cast<unsigned char*>(data);
            op.bytes = long(size);
            op.granulepos = -1;
            op.packetno = d.packetno++;
            if (vorbis_synthesis(&d.vb, &op) == 0 && vorbis_synthesis_blockin(&d.vd, &d.vb) == 0) {
                float** pcm;
                int n;
                std::vector<uint8_t> out;
                while ((n = vorbis_synthesis_pcmout(&d.vd, &pcm)) > 0) {
                    size_t at = out.size();
                    out.resize(at + size_t(n) * d.channels * 2);
                    int16_t* o = reinterpret_cast<int16_t*>(out.data() + at);
                    for (int i = 0; i < n; i++)
                        for (int ch = 0; ch < d.channels; ch++)
                            *o++ = int16_t(std::clamp(pcm[ch][i] * 32767.0f, -32768.0f, 32767.0f));
                    vorbis_synthesis_read(&d.vd, n);
                }
                if (!out.empty()) push(std::move(out), pts, 0), d.frames++;
            }
        }
        if ((flags & kFlagEos) && (d.decoded.empty() || !(d.decoded.back().flags & kFlagEos))) push({}, pts, kFlagEos);
        return;
    }
#endif
#if !defined(NX_NO_VPX)
    if (d.vpx_ok && size) {
        if (vpx_codec_decode(&d.vpx, data, unsigned(size), nullptr, 0) != VPX_CODEC_OK) {
            static int warned = 0;
            if (warned++ < 8) log("movie: frame at %lld us: %s", (long long)pts, vpx_codec_error(&d.vpx));
        }
        vpx_codec_iter_t it = nullptr;
        while (vpx_image_t* img = vpx_codec_get_frame(&d.vpx, &it)) {
            if (img->fmt != VPX_IMG_FMT_I420) continue;  // profile 0 (8-bit 4:2:0) only, what the games ship
            int w = int(img->d_w) & ~1, h = int(img->d_h) & ~1;
            if (!d.width) d.width = w, d.height = h;
            std::vector<uint8_t> nv12(size_t(w) * h * 3 / 2);
            for (int y = 0; y < h; y++)
                std::memcpy(&nv12[size_t(y) * w], img->planes[0] + size_t(y) * img->stride[0], size_t(w));
            uint8_t* uv = nv12.data() + size_t(w) * h;
            for (int y = 0; y < h / 2; y++) {
                const uint8_t* u = img->planes[1] + size_t(y) * img->stride[1];
                const uint8_t* v = img->planes[2] + size_t(y) * img->stride[2];
                uint8_t* o = uv + size_t(y) * w;
                for (int x = 0; x < w / 2; x++) o[2 * x] = u[x], o[2 * x + 1] = v[x];
            }
            push(std::move(nv12), pts, 0);
            d.frames++;
        }
    } else
#endif
        if (size && d.width) {
        std::vector<uint8_t> nv12(size_t(d.width) * d.height * 3 / 2, 16);
        std::fill(nv12.begin() + size_t(d.width) * d.height, nv12.end(), uint8_t(128));
        push(std::move(nv12), pts, 0);
        d.frames++;
    }
    (void)data;
    if ((flags & kFlagEos) && (d.decoded.empty() || !(d.decoded.back().flags & kFlagEos))) {
        push({}, pts, kFlagEos);
        log("movie: end of stream after %llu frames", (unsigned long long)d.frames);
    }
}

// ---- the vtable ------------------------------------------------------------------------------------------------

void Destroy(CPU& c) {  // ~Decoder() (complete)
    uint64_t self = c.x[0];
    std::unique_ptr<Decoder> d;
    {
        std::lock_guard lk(g_lock);
        auto it = g_decoders.find(self);
        if (it != g_decoders.end()) d = std::move(it->second), g_decoders.erase(it);
    }
#if !defined(NX_NO_VPX)
    if (d && d->vpx_ok) vpx_codec_destroy(&d->vpx);
#endif
#if !defined(NX_NO_VORBIS)
    if (d && d->vorbis_ok) {
        vorbis_block_clear(&d->vb);
        vorbis_dsp_clear(&d->vd);
    }
    if (d && d->audio()) vorbis_comment_clear(&d->vc), vorbis_info_clear(&d->vi);
#endif
    // The input buffers stay allocated: kernel memory is not given back (a few MiB per movie played).
    st<uint64_t>(self, 0);
}

void DestroyDelete(CPU& c) {  // ~Decoder() (deleting)
    uint64_t self = c.x[0];
    Destroy(c);
    if (uint64_t del = find_symbol("_ZdlPv")) guest_call(c, del, {self});
}

// The bytes of a MediaData buffer entry ("csd-0", ...): empty when there is none.
std::vector<uint8_t> find_buffer(CPU& c, uint64_t md, const char* k) {
    static const uint64_t find = sym("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
    static const uint64_t data = sym("_ZNK5movie6Buffer4DataEv");
    static const uint64_t size = sym("_ZNK5movie6Buffer4SizeEv");
    uint64_t out = nvn::scratch(c, 16);
    st<uint64_t>(out, 0);
    if (!md || !(guest_call(c, find, {md, key(k), out}) & 1)) return {};
    uint64_t buf = ld<uint64_t>(out);
    if (!buf) return {};
    uint64_t p = guest_call(c, data, {buf}), n = uint32_t(guest_call(c, size, {buf}));
    if (!p || n > (1u << 20)) return {};
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    return std::vector<uint8_t>(b, b + n);
}

// Vorbis: the identification and setup headers from csd-0/csd-1, with an empty comment header between them (the
// decoder needs all three in order; Android's MediaCodec gets only two).
void ConfigureAudio(CPU& c, Decoder& d) {
    std::vector<uint8_t> ident = find_buffer(c, c.x[1], "csd-0"), setup = find_buffer(c, c.x[1], "csd-1");
    std::lock_guard lk(d.lock);
#if !defined(NX_NO_VORBIS)
    vorbis_info_init(&d.vi);
    vorbis_comment_init(&d.vc);
    static const uint8_t comment[] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    auto header = [&](std::vector<uint8_t> bytes, int no) {
        ogg_packet op{};
        op.packet = bytes.data();
        op.bytes = long(bytes.size());
        op.b_o_s = no == 0;
        op.packetno = no;
        return !bytes.empty() && vorbis_synthesis_headerin(&d.vi, &d.vc, &op) == 0;
    };
    bool ok = header(ident, 0) && header(std::vector<uint8_t>(comment, comment + sizeof comment), 1) && header(setup, 2);
    if (ok) ok = vorbis_synthesis_init(&d.vd, &d.vi) == 0 && vorbis_block_init(&d.vd, &d.vb) == 0;
    d.vorbis_ok = ok;
    d.channels = d.vi.channels, d.rate = int(d.vi.rate);
    d.packetno = 3;
    log("movie: Vorbis decoder %s (%d channels, %d Hz; headers %zu + %zu bytes)", ok ? "ready" : "failed", d.channels,
        d.rate, ident.size(), setup.size());
#endif
    c.x[0] = kOk;
}

void Configure(CPU& c) {  // Status (MediaData* format)
    Decoder* d = of(c.x[0]);
    if (!d) return void(c.x[0] = kInvalid);
    if (d->audio()) return ConfigureAudio(c, *d);
    uint64_t out = nvn::scratch(c, 16);
    static const uint64_t find = sym("_ZNK5movie9MediaData9FindInt32EPKcPi");
    int w = 0, h = 0;
    st<int32_t>(out, 0);
    if (c.x[1] && (guest_call(c, find, {c.x[1], key("width"), out}) & 1)) w = ld<int32_t>(out);
    st<int32_t>(out, 0);
    if (c.x[1] && (guest_call(c, find, {c.x[1], key("height"), out}) & 1)) h = ld<int32_t>(out);
    std::lock_guard lk(d->lock);
    d->width = w & ~1, d->height = h & ~1;
    log("movie: configure %dx%d, output format %d", w, h, d->out_format);
    open_codec(*d);
    c.x[0] = kOk;
}

void RegisterClientEvents(CPU& c) {  // Status (DecoderEvents*)
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1]) return void(c.x[0] = kInvalid);
    std::lock_guard lk(d->lock);
    d->events = c.x[1];
    c.x[0] = kOk;
}

void GetOutputFormat(CPU& c) {  // Status (MediaData* out)
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1]) return void(c.x[0] = kInvalid);
    static const uint64_t set32 = sym("_ZN5movie9MediaData8SetInt32EPKci");
    static const uint64_t rect = sym("_ZN5movie9MediaData7SetRectEPKciiii");
    static const uint64_t setstr = sym("_ZN5movie9MediaData9SetStringEPKcS2_");
    int w, h;
    {
        std::lock_guard lk(d->lock);
        w = d->width, h = d->height;
    }
    uint64_t md = c.x[1];
    if (d->audio()) {
        guest_call(c, setstr, {md, key("mime"), key("audio/raw")});
        guest_call(c, set32, {md, key("channel-count"), uint64_t(d->channels)});
        guest_call(c, set32, {md, key("sample-rate"), uint64_t(d->rate)});
        return void(c.x[0] = kOk);
    }
    guest_call(c, setstr, {md, key("mime"), key(d->type == kVp8 ? "video/x-vnd.on2.vp8" : "video/x-vnd.on2.vp9")});
    guest_call(c, set32, {md, key("width"), uint64_t(w)});
    guest_call(c, set32, {md, key("height"), uint64_t(h)});
    guest_call(c, set32, {md, key("stride"), uint64_t(w)});
    guest_call(c, set32, {md, key("slice-height"), uint64_t(h)});
    guest_call(c, rect, {md, key("crop"), 0, 0, uint64_t(w - 1), uint64_t(h - 1)});  // inclusive, as in Android
    c.x[0] = kOk;
}

void Start(CPU& c) {
    Decoder* d = of(c.x[0]);
    if (!d) return void(c.x[0] = kInvalid);
    {
        std::lock_guard lk(d->lock);
        d->started = true;
        d->eos = false;
        d->eos_sent = false;
        d->play_start_ns = 0;
        d->last_copy_ns = 0;
        d->first_pts = 0;
        d->sent = d->frames = d->acquired = d->copied = d->released = 0;
        d->free_inputs.clear();
        for (int k = 0; k < kInputs; k++) {
            if (!d->inputs[k]) d->inputs[k] = kernel_alloc(kInputCapacity);
            d->free_inputs.push_back(k);
        }
        d->free_outputs.clear();
        for (int k = 0; k < d->max_outputs(); k++) {
            d->free_outputs.push_back(k);
        }
    }
    signal(c, *d, 0);
    c.x[0] = kOk;
}

void Flush(CPU& c) {  // also Stop and Reset: drop everything in flight
    Decoder* d = of(c.x[0]);
    if (!d) return void(c.x[0] = kInvalid);
    {
        std::lock_guard lk(d->lock);
        d->decoded.clear();
        d->active_outputs.clear();
        d->current_frame = Frame{};
        d->play_start_ns = 0;
        d->last_copy_ns = 0;
        d->first_pts = 0;
        d->sent = d->frames = d->acquired = d->copied = d->released = 0;
        d->eos = false;
        d->eos_sent = false;
        d->free_inputs.clear();
        for (int k = 0; k < kInputs; k++) d->free_inputs.push_back(k);
        d->free_outputs.clear();
        for (int k = 0; k < d->max_outputs(); k++) d->free_outputs.push_back(k);
    }
    c.x[0] = kOk;
}

void Stop(CPU& c) {
    if (Decoder* d = of(c.x[0])) {
        std::lock_guard lk(d->lock);
        d->started = false;
    }
    Flush(c);
}

void AcquireInputBufferIndex(CPU& c) {  // Status (int* index, int* remaining)
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1] || !c.x[2]) return void(c.x[0] = kInvalid);
    {
        std::lock_guard lk(d->lock);
        st<int32_t>(c.x[1], -1);
        st<int32_t>(c.x[2], 0);
        if (!d->started || d->free_inputs.empty() || d->eos) {
            return void(c.x[0] = kNoBuffer);
        }
        st<int32_t>(c.x[1], d->free_inputs.front());
        d->free_inputs.pop_front();
        int32_t remaining = int32_t(d->free_inputs.size());
        st<int32_t>(c.x[2], remaining);
    }
    c.x[0] = kOk;
}

void GetInputBuffer(CPU& c) {  // Status (int index, Buffer*)
    Decoder* d = of(c.x[0]);
    int index = int(c.x[1]);
    if (!d || !c.x[2] || index < 0 || index >= kInputs || !d->inputs[index]) return void(c.x[0] = kInvalid);
    static const uint64_t set_data = sym("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    static const uint64_t set_range = sym("_ZN5movie6Buffer8SetRangeEii");
    guest_call(c, set_data, {c.x[2], d->inputs[index], kInputCapacity});
    guest_call(c, set_range, {c.x[2], 0, 0});
    c.x[0] = kOk;
}

// Status (int index, int offset, int size, int64_t presentation time (us), uint32_t flags)
void SendInputBufferForDecode(CPU& c) {
    Decoder* d = of(c.x[0]);
    int index = int(c.x[1]), offset = int(c.x[2]), size = int(c.x[3]);
    int64_t pts = int64_t(c.x[4]);
    uint32_t flags = uint32_t(c.x[5]);
    if (!d || index < 0 || index >= kInputs || offset < 0 || size < 0 || uint64_t(offset) + size > kInputCapacity)
        return void(c.x[0] = kInvalid);
    // Gentle real-time backpressure on the demuxer thread while video/audio is actively playing:
    // keeps decoded queue bounded during playback, but never blocks if Skip is pressed.
    for (int wait = 0; wait < 16; wait++) {
        {
            std::lock_guard lk(d->lock);
            if (!d->started || d->eos) break;
            if (d->audio()) {
                if (d->released == 0 || d->decoded.size() < 24) break;
            } else {
                uint64_t now = plat_now_ns();
                bool rendering_active = d->play_start_ns != 0 && (now - d->last_copy_ns) < 60'000'000ull;
                if (!rendering_active || d->decoded.size() < 12) break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool format_changed = false, input_back = false, output = false;
    {
        std::lock_guard lk(d->lock);
        if (!d->started) return void(c.x[0] = kOk);
        size_t before = d->decoded.size();
        decode(*d, reinterpret_cast<const uint8_t*>(d->inputs[index] + offset), size_t(size), pts, flags);
        if (d->sent++ % 300 == 0)
            log("movie: %s input %llu: %d bytes at %lld us, flags %#x; %llu frames decoded, %zu decoded waiting",
                d->audio() ? "audio" : "video", (unsigned long long)d->sent, size, (long long)pts, flags,
                (unsigned long long)d->frames, d->decoded.size());
        output = (d->decoded.size() > before) && !d->free_outputs.empty() && !d->eos_sent;
        if (d->decoded.size() > before && !d->format_sent && (d->width || d->channels)) d->format_sent = format_changed = true;
        if (flags & kFlagEos) d->eos = true;
        d->free_inputs.push_back(index);
        input_back = d->started && !d->eos;
    }
    if (format_changed) signal(c, *d, 2);
    if (output) signal(c, *d, 1);
    if (input_back) signal(c, *d, 0);
    c.x[0] = kOk;
}

// Status (int* index, int64_t* presentation time, uint32_t* flags, int* remaining)
void AcquireOutputBufferIndex(CPU& c) {
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1] || !c.x[2] || !c.x[3] || !c.x[4]) return void(c.x[0] = kInvalid);
    {
        std::lock_guard lk(d->lock);
        st<int32_t>(c.x[1], -1);
        st<int64_t>(c.x[2], 0);
        st<uint32_t>(c.x[3], 0);
        st<int32_t>(c.x[4], 0);
        if (!d->started || d->free_outputs.empty() || d->eos_sent) return void(c.x[0] = kNoOutputBuffer);

        if (d->audio()) {
            if (d->decoded.empty()) return void(c.x[0] = kNoOutputBuffer);
            if (d->decoded.front().flags & kFlagEos) {
                // Deliver audio EOS only once all previously acquired audio buffers have been released.
                if (int(d->free_outputs.size()) < d->max_outputs()) return void(c.x[0] = kNoOutputBuffer);
                Frame f = std::move(d->decoded.front());
                d->decoded.pop_front();
                int idx = d->free_outputs.front();
                d->free_outputs.pop_front();
                f.index = idx;
                f.acquired = true;
                d->eos_sent = true;
                d->acquired++;
                st<int32_t>(c.x[1], idx);
                st<int64_t>(c.x[2], f.pts);
                st<uint32_t>(c.x[3], kFlagEos);
                st<int32_t>(c.x[4], 0);
                d->active_outputs[idx] = std::move(f);
                return void(c.x[0] = kOk);
            }
            Frame f = std::move(d->decoded.front());
            d->decoded.pop_front();
            int idx = d->free_outputs.front();
            d->free_outputs.pop_front();
            f.index = idx;
            f.acquired = true;
            d->acquired++;
            int32_t remaining = int32_t(std::min(d->free_outputs.size(), d->decoded.size()));
            if (!d->decoded.empty() && (d->decoded.front().flags & kFlagEos) && int(d->free_outputs.size()) < d->max_outputs()) {
                remaining = 0;
            }
            st<int32_t>(c.x[1], idx);
            st<int64_t>(c.x[2], f.pts);
            st<uint32_t>(c.x[3], f.flags);
            st<int32_t>(c.x[4], remaining);
            d->active_outputs[idx] = std::move(f);
        } else {
            if (d->decoded.empty() && d->current_frame.data.empty()) return void(c.x[0] = kNoOutputBuffer);
            // Video EOS: deliver only after GetOutputBuffer has consumed all real frames and reached the final frame's end.
            if (!d->decoded.empty() && (d->decoded.front().flags & kFlagEos) && d->play_start_ns != 0) {
                int64_t elapsed_us = d->first_pts + int64_t((plat_now_ns() - d->play_start_ns) / 1000);
                if (elapsed_us >= d->current_frame.pts + 16'000) {
                    Frame f = std::move(d->decoded.front());
                    d->decoded.pop_front();
                    int idx = d->free_outputs.front();
                    d->free_outputs.pop_front();
                    f.index = idx;
                    f.acquired = true;
                    d->eos_sent = true;
                    d->acquired++;
                    st<int32_t>(c.x[1], idx);
                    st<int64_t>(c.x[2], f.pts);
                    st<uint32_t>(c.x[3], kFlagEos);
                    st<int32_t>(c.x[4], 0);
                    d->active_outputs[idx] = std::move(f);
                    return void(c.x[0] = kOk);
                }
            }
            int idx = d->free_outputs.front();
            d->free_outputs.pop_front();
            int64_t pts = d->current_frame.pts;
            if (d->play_start_ns != 0) {
                int64_t elapsed_us = d->first_pts + int64_t((plat_now_ns() - d->play_start_ns) / 1000);
                pts = std::max(pts, elapsed_us);
            }
            Frame f;
            f.pts = pts;
            f.flags = 0;
            f.acquired = true;
            f.index = idx;
            d->active_outputs[idx] = std::move(f);
            d->acquired++;
            int32_t remaining = int32_t(d->free_outputs.size());
            st<int32_t>(c.x[1], idx);
            st<int64_t>(c.x[2], pts);
            st<uint32_t>(c.x[3], 0);
            st<int32_t>(c.x[4], remaining);
        }
    }
    c.x[0] = kOk;
}

void GetOutputBuffer(CPU& c) {  // Status (int index, Buffer* out): the frame copied into the game's buffer
    Decoder* d = of(c.x[0]);
    uint64_t buf = c.x[2];
    if (!d || !buf) return void(c.x[0] = kInvalid);
    static const uint64_t base = sym("_ZNK5movie6Buffer4BaseEv");
    static const uint64_t capacity = sym("_ZNK5movie6Buffer8CapacityEv");
    static const uint64_t set_range = sym("_ZN5movie6Buffer8SetRangeEii");
    uint64_t dst = guest_call(c, base, {buf}), cap = guest_call(c, capacity, {buf});
    size_t n = 0;
    {
        std::lock_guard lk(d->lock);
        auto it = d->active_outputs.find(int(c.x[1]));
        if (it == d->active_outputs.end()) return void(c.x[0] = kInvalid);
        if (d->audio()) {
            const Frame& f = it->second;
            n = cap > 0 ? std::min<size_t>(f.data.size(), cap) : f.data.size();
            if (n && dst) std::memcpy(reinterpret_cast<void*>(dst), f.data.data(), n);
            if (d->copied++ % 300 == 0)
                log("movie: audio frame %llu at %lld us -> buffer %llx (capacity %llu, %zu of %zu bytes); %llu sent, %llu decoded, "
                    "%llu acquired, %llu released", (unsigned long long)d->copied, (long long)f.pts,
                    (unsigned long long)dst, (unsigned long long)cap, n, f.data.size(), (unsigned long long)d->sent,
                    (unsigned long long)d->frames, (unsigned long long)d->acquired, (unsigned long long)d->released);
        } else {
            uint64_t now = plat_now_ns();
            d->last_copy_ns = now;
            if (d->play_start_ns == 0) {
                d->play_start_ns = now;
                if (!d->decoded.empty() && !(d->decoded.front().flags & kFlagEos)) {
                    d->first_pts = d->decoded.front().pts;
                    d->current_frame = std::move(d->decoded.front());
                    d->decoded.pop_front();
                } else {
                    d->first_pts = d->current_frame.pts;
                }
            } else {
                int64_t elapsed_us = d->first_pts + int64_t((now - d->play_start_ns) / 1000);
                while (!d->decoded.empty() && !(d->decoded.front().flags & kFlagEos) &&
                       d->decoded.front().pts <= elapsed_us) {
                    d->current_frame = std::move(d->decoded.front());
                    d->decoded.pop_front();
                }
            }
            const Frame& f = d->current_frame;
            n = cap > 0 ? std::min<size_t>(f.data.size(), cap) : f.data.size();
            if (n && dst) std::memcpy(reinterpret_cast<void*>(dst), f.data.data(), n);
            if (d->copied++ % 300 == 0)
                log("movie: video frame %llu at %lld us -> buffer %llx (capacity %llu, %zu of %zu bytes); %llu sent, %llu decoded, "
                    "%llu acquired, %llu released", (unsigned long long)d->copied, (long long)f.pts,
                    (unsigned long long)dst, (unsigned long long)cap, n, f.data.size(), (unsigned long long)d->sent,
                    (unsigned long long)d->frames, (unsigned long long)d->acquired, (unsigned long long)d->released);
        }
    }
    guest_call(c, set_range, {buf, 0, n});
    if (n && dst && !d->audio()) textures_written(dst, n);
    c.x[0] = kOk;
}

void ReleaseOutputBufferIndex(CPU& c) {  // Status (int index)
    Decoder* d = of(c.x[0]);
    if (!d) return void(c.x[0] = kInvalid);
    int index = int(c.x[1]);
    bool output_avail = false;
    {
        std::lock_guard lk(d->lock);
        auto it = d->active_outputs.find(index);
        if (it != d->active_outputs.end()) {
            d->active_outputs.erase(it);
            d->free_outputs.push_back(index);
            d->released++;
        }
        if (!d->started) return void(c.x[0] = kOk);
        if (d->audio()) {
            output_avail = !d->eos_sent && !d->decoded.empty() && !d->free_outputs.empty();
            if (output_avail && (d->decoded.front().flags & kFlagEos) && int(d->free_outputs.size()) < d->max_outputs()) {
                output_avail = false;
            }
        } else {
            output_avail = !d->eos_sent && (!d->decoded.empty() || !d->current_frame.data.empty()) && !d->free_outputs.empty();
        }
    }
    if (output_avail) signal(c, *d, 1);
    c.x[0] = kOk;
}

void GetOutputBufferSize(CPU& c) {  // Status (int* size, int* count)
    Decoder* d = of(c.x[0]);
    if (!d || !c.x[1] || !c.x[2]) return void(c.x[0] = kInvalid);
    std::lock_guard lk(d->lock);
    int w = d->width > 0 ? d->width : 1920;
    int h = d->height > 0 ? d->height : 1080;
    st<int32_t>(c.x[1], d->audio() ? 0x2000 * std::max(d->channels, 1) : (w * h * 3 / 2));
    st<int32_t>(c.x[2], d->max_outputs());
    c.x[0] = kOk;
}

void GetLastError(CPU& c) { c.x[0] = kOk; }
void GetOutputBufferWithFormat(CPU& c) {  // Status (int index, Buffer*, MediaData*)
    uint64_t self = c.x[0], md = c.x[3];
    GetOutputBuffer(c);
    if (c.x[0] == kOk && md) {
        uint64_t saved = c.x[1];
        c.x[0] = self, c.x[1] = md;
        GetOutputFormat(c);
        c.x[1] = saved;
    }
}
void GetOutputBufferFormat(CPU& c) {  // Status (int index, MediaData*)
    c.x[1] = c.x[2];
    GetOutputFormat(c);
}

// NX_MOVIE_TRACE=1: every call of every slot (else the first of each).
template <HleFn F>
void traced(CPU& c) {
    static const bool all = std::getenv("NX_MOVIE_TRACE") != nullptr;
    static std::atomic<int> n{0};
    uint64_t a1 = c.x[1], a2 = c.x[2];
    Decoder* d = of(c.x[0]);
    const char* who = !d ? "?" : d->audio() ? "audio" : "video";
    if (all || n++ == 0)
        log("movie: %s %s(%llx, %llx)", who, hle_current_name(), (unsigned long long)a1, (unsigned long long)a2);
    F(c);
    if (all) log("movie:   %s -> %llx", who, (unsigned long long)c.x[0]);
}

// The slots after the address point, in movie::Decoder's order.
struct Slot {
    const char* name;
    HleFn fn;
};
constexpr Slot kSlots[] = {
    {"movie::Decoder::~Decoder", traced<Destroy>},
    {"movie::Decoder::~Decoder(deleting)", traced<DestroyDelete>},
    {"movie::Decoder::Configure", traced<Configure>},
    {"movie::Decoder::RegisterClientEvents", traced<RegisterClientEvents>},
    {"movie::Decoder::GetOutputFormat", traced<GetOutputFormat>},
    {"movie::Decoder::Start", traced<Start>},
    {"movie::Decoder::Stop", traced<Stop>},
    {"movie::Decoder::Reset", traced<Flush>},
    {"movie::Decoder::Flush", traced<Flush>},
    {"movie::Decoder::AcquireInputBufferIndex", traced<AcquireInputBufferIndex>},
    {"movie::Decoder::GetInputBuffer", traced<GetInputBuffer>},
    {"movie::Decoder::SendInputBufferForDecode", traced<SendInputBufferForDecode>},
    {"movie::Decoder::AcquireOutputBufferIndex", traced<AcquireOutputBufferIndex>},
    {"movie::Decoder::GetOutputBuffer", traced<GetOutputBuffer>},
    {"movie::Decoder::GetOutputBufferSize", traced<GetOutputBufferSize>},
    {"movie::Decoder::ReleaseOutputBufferIndex", traced<ReleaseOutputBufferIndex>},
    {"movie::Decoder::GetLastError", traced<GetLastError>},
    {"movie::Decoder::GetOutputBuffer(format)", traced<GetOutputBufferWithFormat>},
    {"movie::Decoder::GetOutputBufferFormat", traced<GetOutputBufferFormat>},
};

uint64_t vtable() {
    static std::once_flag once;
    std::call_once(once, [] {
        uint64_t vt = kernel_alloc(0x1000);
        st<uint64_t>(vt, 0);      // offset to top
        st<uint64_t>(vt + 8, 0);  // typeinfo: none (the game does not use RTTI on it)
        int k = 0;
        for (const Slot& s : kSlots) st<uint64_t>(vt + 16 + 8 * k++, hle_address_or(s.name, s.fn));
        g_vtable = vt + 16;
    });
    return g_vtable;
}

// Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int thread priority)
void Construct(CPU& c) {
    uint64_t self = c.x[0];
    int type = int(c.x[1]), format = int(c.x[2]);
#if !defined(NX_NO_VORBIS)
    const bool ours = type == kVp8 || type == kVp9 || type == kVorbis;
#else
    const bool ours = type == kVp8 || type == kVp9;
#endif
    if (!ours) {
        static const uint64_t real = sym(kCtor);
        call(c, real);
        return;
    }
    auto d = std::make_unique<Decoder>();
    d->type = type, d->out_format = format;
    if (type != kVorbis && format != 4) log("movie: video output format %d asked; NV12 (4) given", format);
    if (type == kVorbis && format != 1) log("movie: audio output format %d asked; PCM16 (1) given", format);
    st<uint64_t>(self, vtable());
    st<uint64_t>(self + 8, 0);
    std::lock_guard lk(g_lock);
    g_decoders[self] = std::move(d);
}

}  // namespace

NX_HLE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", Construct);

}  // namespace nx
