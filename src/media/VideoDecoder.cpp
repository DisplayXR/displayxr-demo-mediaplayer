// SPDX-License-Identifier: Apache-2.0
#include "media/VideoDecoder.h"

#include "Log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

// Include the D3D/DXGI C++ headers first (with C++ linkage): FFmpeg's
// hwcontext_d3d11va.h pulls in <d3d11.h>, and if that first happens inside the
// extern "C" block below it mis-declares d3d11.h's operator==/!= as C functions.
#if defined(_WIN32)
#include <d3d11.h>
#include <d3dcompiler.h>  // D3DCompile: NV12->RGBA convert shaders (#28)
#include <dxgi1_2.h>
#endif

#if defined(MEDIAPLAYER_WITH_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>  // <d3d11.h> already included above (guarded)
#endif
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace mp {

#if defined(MEDIAPLAYER_WITH_FFMPEG)

// Passed via AVCodecContext::opaque to the get_format hook. Carries the wanted GPU
// surface format plus write-backs so the hook can record a software fallback for the
// HUD/logs. Defined before Impl so Impl can embed one.
struct HwFormatHook {
    enum AVPixelFormat want = AV_PIX_FMT_NONE;  // the decoder's GPU surface format
    bool* hardware = nullptr;                   // cleared if we fall back to software
    const char** backendName = nullptr;         // retargeted to "software …" on fallback
};

struct VideoDecoder::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;                  // only for the odd-format -> I420 fallback
    AVBufferRef* hwDevice = nullptr;            // hwaccel device (null = software decode)
    AVFrame* swFrame = nullptr;                 // download target for hardware frames
    AVFrame* convFrame = nullptr;               // I420 fallback target (rare formats)
    enum AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;  // the decoder's GPU surface format
    HwFormatHook hook;                          // ctx->opaque for the get_format callback
    int videoStream = -1;
    int swsW = 0, swsH = 0, swsFmt = -1;        // cached sws source params (fallback)

#if defined(_WIN32)
    // Zero-copy interop (#28): non-owning handles into the D3D11VA device FFmpeg decodes
    // on (owned via hwDevice). d3dHwctx->lock/unlock guards the shared context.
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    AVD3D11VADeviceContext* d3dHwctx = nullptr;
    ID3D11Query* copyDoneQuery = nullptr;  // event query: producer waits for the copy (#28)

    // NV12->RGBA convert pass (#28). Vulkan can't sample an imported D3D11 multiplanar NV12
    // (the driver's UV-plane alignment differs from D3D's tight pack), so we convert to a
    // single packed RGBA surface on the D3D side and share that — a plain single-plane image
    // Vulkan imports at offset 0 with no alignment constraint. Built lazily, reused per stream.
    ID3D11VertexShader* convVS = nullptr;
    ID3D11PixelShader* convPS = nullptr;
    ID3D11SamplerState* convSampler = nullptr;
    ID3D11RasterizerState* convRaster = nullptr;    // cull-none, solid
    ID3D11Buffer* convCB = nullptr;                 // { float fullRange; } per-frame
    ID3D11Texture2D* nv12Intermediate = nullptr;    // SRV-able copy target for the decoded slice
    ID3D11ShaderResourceView* srvY = nullptr;       // R8   over plane 0
    ID3D11ShaderResourceView* srvUV = nullptr;      // R8G8 over plane 1
    int convW = 0, convH = 0;                       // cached intermediate dims
#endif

    ~Impl() {
#if defined(_WIN32)
        if (copyDoneQuery) copyDoneQuery->Release();
        if (srvUV) srvUV->Release();
        if (srvY) srvY->Release();
        if (nv12Intermediate) nv12Intermediate->Release();
        if (convCB) convCB->Release();
        if (convRaster) convRaster->Release();
        if (convSampler) convSampler->Release();
        if (convPS) convPS->Release();
        if (convVS) convVS->Release();
#endif
        if (sws) sws_freeContext(sws);
        if (swFrame) av_frame_free(&swFrame);
        if (convFrame) av_frame_free(&convFrame);
        if (codec) avcodec_free_context(&codec);
        if (hwDevice) av_buffer_unref(&hwDevice);
        if (fmt) avformat_close_input(&fmt);
    }
};

namespace {

// Per-OS hwaccel preference, tried in order; the first that initializes wins. The
// list is empty on unknown platforms, which simply yields the software path.
const AVHWDeviceType* PreferredHwTypes(size_t& count) {
#if defined(__APPLE__)
    static const AVHWDeviceType t[] = {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#elif defined(_WIN32)
    // D3D11VA covers Intel/AMD/NVIDIA; CUDA is the NVDEC path for NVIDIA-only stacks.
    static const AVHWDeviceType t[] = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA};
#elif defined(__linux__)
    static const AVHWDeviceType t[] = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA};
#else
    static const AVHWDeviceType t[] = {AV_HWDEVICE_TYPE_NONE};  // none usable
#endif
    count = (t[0] == AV_HWDEVICE_TYPE_NONE) ? 0 : sizeof(t) / sizeof(t[0]);
    return t;
}

// get_format callback: keep the GPU surface format when the decoder offers it, so
// frames stay on the device until we explicitly download them. opaque points at the
// Impl's hwPixFmt (set before avcodec_open2), keeping this free of the private type.
enum AVPixelFormat PickHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    auto* hook = static_cast<HwFormatHook*>(ctx->opaque);
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == hook->want) return *p;
    }
    // The GPU surface format isn't on offer — the runtime hwaccel failed to initialise
    // for this stream (commonly: H.264 wider than the GPU's 4096px DXVA decode limit,
    // as with 5120-wide SBS clips). Returning AV_PIX_FMT_NONE would *abort* the decoder
    // (black video); instead fall back to software so playback continues. frame->format
    // then won't match hwPixFmt, so DecodeLoop skips the download and swscale runs on the
    // CPU frame directly. The flag writes race a HUD read but are word-sized + write-once.
    LOG_WARN("VideoDecoder: hw surface unavailable for this stream — decoding in software");
    if (hook->hardware) *hook->hardware = false;
    if (hook->backendName) *hook->backendName = "software (hw unsupported)";
    return avcodec_default_get_format(ctx, fmts);
}

// Find the decoder's GPU pixel format for `type`, if this build/codec supports it.
enum AVPixelFormat HwPixFmtFor(const AVCodec* dec, AVHWDeviceType type) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
        if (!cfg) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == type) {
            return cfg->pix_fmt;
        }
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { Stop(); }

#if defined(_WIN32)
void VideoDecoder::SetInteropAdapterLUID(const uint8_t* luid8) {
    std::memcpy(interopLUID_, luid8, 8);
    haveInteropLUID_ = true;
}

namespace {
// Build an FFmpeg D3D11VA hwdevice context whose ID3D11Device lives on the adapter with
// the given LUID, so decoded NV12 surfaces can later be shared into Vulkan on the SAME
// GPU with no CPU round trip (issue #28). Returns nullptr on any failure — the caller
// then falls back to FFmpeg's own default-adapter device (still hardware, just no interop).
// On success FFmpeg owns the device + context (released when the hwdevice ref is freed).
AVBufferRef* CreateD3D11VADeviceOnLUID(const uint8_t luid[8]) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return nullptr;
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* it = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &it) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(it->GetDesc1(&desc)) && std::memcmp(&desc.AdapterLuid, luid, sizeof(LUID)) == 0) {
            chosen = it;  // keep this ref
            break;
        }
        it->Release();
    }
    factory->Release();
    if (!chosen) return nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr,
                                         0, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    chosen->Release();
    if (FAILED(hr)) return nullptr;

    // D3D11VA + cross-thread sharing require a multithread-protected context.
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    AVBufferRef* hwref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwref) { ctx->Release(); device->Release(); return nullptr; }
    auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(hwref->data);
    auto* d3d = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
    d3d->device = device;           // ownership transferred to FFmpeg
    d3d->device_context = ctx;      // ditto
    if (av_hwdevice_ctx_init(hwref) < 0) { av_buffer_unref(&hwref); return nullptr; }
    return hwref;
}

// Create a shareable RGBA render-target texture on `device`, w x h, plus its shared KMT
// handle for Vulkan import. Returns false on failure. Caller owns tex (Release); the KMT
// handle is owned by the resource (freed by its Release, not CloseHandle).
bool CreateSharedRGBA(ID3D11Device* device, int w, int h, ID3D11Texture2D** outTex,
                      void** outHandle) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // convert draws into it
    // Legacy shared handle (D3D11_RESOURCE_MISC_SHARED -> a KMT handle), no keyed mutex.
    // Cross-API coherence is the producer CPU-waiting on an event query before publishing.
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &tex))) return false;
    IDXGIResource* res = nullptr;
    if (FAILED(tex->QueryInterface(__uuidof(IDXGIResource), (void**)&res))) {
        tex->Release();
        return false;
    }
    HANDLE handle = nullptr;
    const HRESULT hr = res->GetSharedHandle(&handle);  // KMT handle, owned by the resource
    res->Release();
    if (FAILED(hr) || !handle) {
        tex->Release();
        return false;
    }
    *outTex = tex;
    *outHandle = handle;
    return true;
}

// Fullscreen NV12->RGB convert shaders (BT.709, range-aware — matches shaders/sbs.frag).
// texY = R8 plane 0, texUV = R8G8 plane 1; cbuffer carries full-vs-limited range.
static const char* kConvertHLSL = R"(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState samp : register(s0);
cbuffer Params : register(b0) { float uFullRange; float3 _pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vsMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 psMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float y = texY.Sample(samp, uv).r;
    float2 c = texUV.Sample(samp, uv).rg;
    float u = c.r, v = c.g;
    float yb, ub, vb;
    if (uFullRange > 0.5) { yb = y; ub = u - 0.5; vb = v - 0.5; }
    else {
        yb = (y - 16.0 / 255.0) * (255.0 / 219.0);
        ub = (u - 128.0 / 255.0) * (255.0 / 224.0);
        vb = (v - 128.0 / 255.0) * (255.0 / 224.0);
    }
    float3 rgb = float3(yb + 1.5748 * vb, yb - 0.1873 * ub - 0.4681 * vb, yb + 1.8556 * ub);
    return float4(saturate(rgb), 1.0);
}
)";

bool CompileConvertShaders(ID3D11Device* device, ID3D11VertexShader** outVS,
                           ID3D11PixelShader** outPS) {
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* err = nullptr;
    const size_t len = std::strlen(kConvertHLSL);
    if (FAILED(D3DCompile(kConvertHLSL, len, "convert", nullptr, nullptr, "vsMain", "vs_5_0",
                          flags, 0, &vsb, &err))) {
        if (err) LOG_WARN("convert VS compile: %s", (const char*)err->GetBufferPointer());
        if (err) err->Release();
        return false;
    }
    if (FAILED(D3DCompile(kConvertHLSL, len, "convert", nullptr, nullptr, "psMain", "ps_5_0",
                          flags, 0, &psb, &err))) {
        if (err) LOG_WARN("convert PS compile: %s", (const char*)err->GetBufferPointer());
        if (err) err->Release();
        vsb->Release();
        return false;
    }
    const bool ok =
        SUCCEEDED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                             nullptr, outVS)) &&
        SUCCEEDED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                            nullptr, outPS));
    vsb->Release();
    psb->Release();
    return ok;
}
} // namespace
#endif  // _WIN32

// Attach a hwaccel device to impl_->codec, trying each preferred type. On success
// sets hardware_/hwName_/hwPixFmt and the get_format hook; returns false (software)
// if none initialize (e.g. headless box, codec unsupported by the GPU).
bool VideoDecoder::TryEnableHwAccel(const void* decoder) {
    const AVCodec* dec = static_cast<const AVCodec*>(decoder);
    if (const char* f = std::getenv("MEDIAPLAYER_FORCE_SOFTWARE"); f && *f && *f != '0') {
        LOG_INFO("VideoDecoder: MEDIAPLAYER_FORCE_SOFTWARE set — skipping hwaccel");
        return false;
    }
    size_t n = 0;
    const AVHWDeviceType* types = PreferredHwTypes(n);
    for (size_t i = 0; i < n; ++i) {
        const AVHWDeviceType type = types[i];
        const enum AVPixelFormat pf = HwPixFmtFor(dec, type);
        if (pf == AV_PIX_FMT_NONE) continue;  // codec can't use this device in this build

        AVBufferRef* dev = nullptr;
#if defined(_WIN32)
        // Prefer a D3D11 device pinned to the Vulkan adapter so decoded surfaces can be
        // shared into Vulkan zero-copy (issue #28). Falls through to FFmpeg's own device
        // if the LUID isn't set or the pinned device can't be created.
        if (type == AV_HWDEVICE_TYPE_D3D11VA && haveInteropLUID_ && !live_) {  // #93: no zero-copy live
            dev = CreateD3D11VADeviceOnLUID(interopLUID_);
            if (dev) {
                interopActive_ = true;
                LOG_INFO("VideoDecoder: D3D11VA device pinned to Vulkan adapter (zero-copy #28)");
            } else {
                LOG_WARN("VideoDecoder: could not pin D3D11 device to Vulkan adapter; "
                         "using default device (no zero-copy)");
            }
        }
#endif
        if (!dev && av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) {
            LOG_WARN("VideoDecoder: hwaccel '%s' present but device init failed",
                     av_hwdevice_get_type_name(type));
            continue;
        }

        impl_->hwDevice = dev;
        impl_->hwPixFmt = pf;
        impl_->hook = {pf, &hardware_, &hwName_};  // read/written by PickHwFormat
        impl_->codec->hw_device_ctx = av_buffer_ref(dev);
        impl_->codec->opaque = &impl_->hook;
        impl_->codec->get_format = PickHwFormat;
        hwName_ = av_hwdevice_get_type_name(type);
        hardware_ = true;
        return true;
    }
    return false;
}

bool VideoDecoder::Open(const std::string& path) {
    impl_ = std::make_unique<Impl>();

    if (avformat_open_input(&impl_->fmt, path.c_str(), nullptr, nullptr) < 0) {
        LOG_ERROR("VideoDecoder: cannot open '%s'", path.c_str());
        impl_.reset();
        return false;
    }
    if (avformat_find_stream_info(impl_->fmt, nullptr) < 0) {
        LOG_ERROR("VideoDecoder: no stream info in '%s'", path.c_str());
        impl_.reset();
        return false;
    }

#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVCodec* dec = nullptr;
#else
    AVCodec* dec = nullptr;  // ffmpeg 4.x (Ubuntu 22.04): out-param not yet const
#endif
    impl_->videoStream = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (impl_->videoStream < 0 || !dec) {
        LOG_ERROR("VideoDecoder: no video stream in '%s'", path.c_str());
        impl_.reset();
        return false;
    }
    codecName_ = dec->name;
    if (!OpenVideoCodec(dec, path)) {
        impl_.reset();
        return false;
    }
    AVStream* st = impl_->fmt->streams[impl_->videoStream];

    width_ = impl_->codec->width;
    height_ = impl_->codec->height;
    frameRate_ = av_q2d(st->avg_frame_rate);
    if (frameRate_ <= 0.0) frameRate_ = av_q2d(st->r_frame_rate);
    durationSec_ = (impl_->fmt->duration > 0)
                       ? (double)impl_->fmt->duration / (double)AV_TIME_BASE
                       : 0.0;
    positionSec_.store(0.0);
    seekRequest_.store(-1.0);
    ended_.store(false);
    open_ = true;
    LOG_INFO("VideoDecoder: '%s' %dx%d codec=%s backend=%s dur=%.1fs", path.c_str(), width_,
             height_, codecName_, hwName_, durationSec_);

#if defined(_WIN32)
    // Cache the D3D11 device/context for the zero-copy copy path (#28).
    if (interopActive_ && impl_->hwDevice) {
        auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(impl_->hwDevice->data);
        impl_->d3dHwctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
        impl_->d3dDevice = impl_->d3dHwctx->device;
        impl_->d3dContext = impl_->d3dHwctx->device_context;
        LOG_INFO("VideoDecoder: zero-copy interop path active (NV12->RGBA convert, KMT handle + "
                 "event-query coherence)");
    }
#endif

    stop_ = false;
    thread_ = std::thread(&VideoDecoder::DecodeLoop, this);
    return true;
}

bool VideoDecoder::OpenVideoCodec(const void* decoder, const std::string& label) {
    const AVCodec* dec = static_cast<const AVCodec*>(decoder);
    AVStream* st = impl_->fmt->streams[impl_->videoStream];

    // Build the codec context; try hardware decode, fall back to software if either
    // the device won't init or the hw-configured decoder won't open.
    impl_->codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(impl_->codec, st->codecpar);
    const bool wantHw = TryEnableHwAccel(dec);
    // Always allow auto multithreading. Hardware decode ignores thread_count; but when a
    // stream is too wide for the GPU (PickHwFormat falls back to software PER FRAME), this
    // is what keeps that fallback multithreaded instead of crawling on one core — the case
    // for 7680-wide SBS clips, where single-threaded software decode misses the budget.
    impl_->codec->thread_count = 0;
    // Live (#93): output each frame as soon as it is decodable (no frame-thread delay
    // queue beyond what the codec needs).
    if (live_) impl_->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;

    int rc = avcodec_open2(impl_->codec, dec, nullptr);
    if (rc < 0 && wantHw) {
        LOG_WARN("VideoDecoder: hwaccel '%s' failed to open decoder; retrying software",
                 hwName_);
        // Drop the hw context entirely and rebuild a clean software decoder.
        avcodec_free_context(&impl_->codec);
        av_buffer_unref(&impl_->hwDevice);
        impl_->hwPixFmt = AV_PIX_FMT_NONE;
        hardware_ = false;
        hwName_ = "software";
#if defined(_WIN32)
        interopActive_ = false;  // no hw device -> no zero-copy
#endif
        impl_->codec = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(impl_->codec, st->codecpar);
        impl_->codec->thread_count = 0;
        if (live_) impl_->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        rc = avcodec_open2(impl_->codec, dec, nullptr);
    }
    if (rc < 0) {
        LOG_ERROR("VideoDecoder: cannot open decoder for '%s'", label.c_str());
        return false;
    }
    return true;
}

void VideoDecoder::DecodeLoop() {
    using clock = std::chrono::steady_clock;
    // Live (#93): impl_ does not exist yet — LiveLoop connects on this thread and
    // re-points timeBase at each connection's stream.
    double timeBase = 0.0;
    if (!live_) timeBase = av_q2d(impl_->fmt->streams[impl_->videoStream]->time_base);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto wallStart = clock::now();
    double firstPtsSec = -1.0;
    bool audioGiveUp = false;  // latched when the audio clock ends/stalls -> wall-clock pacing

    // Copy one plane (stride-aware) into a tightly packed buffer.
    auto copyPlane = [](std::vector<uint8_t>& d, const uint8_t* s, int stride, int wBytes, int h) {
        d.resize((size_t)wBytes * h);
        if (stride == wBytes) {
            std::memcpy(d.data(), s, (size_t)wBytes * h);
        } else {
            for (int y = 0; y < h; ++y)
                std::memcpy(d.data() + (size_t)y * wBytes, s + (size_t)y * stride, wBytes);
        }
    };

    // Download a HW frame if needed, then hand the decoder's NATIVE planar YUV to the
    // ring (no RGBA convert, no downscale — the GPU does both, off this thread). Returns
    // PTS in seconds (-1 unknown, -2 = drop/HW fail). I420 (software) and NV12 (hw) are
    // copied directly; any other format (10-bit, 4:2:2/4:4:4) is swscale'd to I420 first.
    auto fillFrame = [&](AVFrame* f) -> double {
#if defined(_WIN32)
        // Zero-copy (#28): the decoded surface is already an NV12 D3D11 texture on the Vulkan
        // adapter. We convert it to a packed RGBA surface on the D3D side (CopySubresourceRegion
        // the decoder slice into an SRV-able NV12, then a fullscreen NV12->RGB draw into this
        // slot's shared RGBA texture) and hand the renderer that handle — Vulkan imports a plain
        // single-plane RGBA image (no multiplanar plane-alignment mismatch). Skips the CPU
        // download/convert/upload. Any failure disables interop and falls through to the CPU path.
        if (interopActive_ && impl_->hwPixFmt != AV_PIX_FMT_NONE && f->format == impl_->hwPixFmt &&
            impl_->d3dContext && impl_->d3dHwctx) {
            FrameRing::Frame& dst = ring_.WriteBuffer();
            // (Re)create this slot's shared RGBA texture on first use or a size change.
            if (dst.gpuSharedTexture && (dst.width != f->width || dst.height != f->height)) {
                reinterpret_cast<ID3D11Texture2D*>(dst.gpuSharedTexture)->Release();
                dst.gpuSharedTexture = nullptr;  // KMT handle freed by the Release, not closed
                dst.gpuSharedHandle = nullptr;
            }
            if (!dst.gpuSharedTexture) {
                ID3D11Texture2D* t = nullptr;
                void* hnd = nullptr;
                if (CreateSharedRGBA(impl_->d3dDevice, f->width, f->height, &t, &hnd)) {
                    dst.gpuSharedTexture = t;
                    dst.gpuSharedHandle = hnd;
                } else {
                    LOG_WARN("VideoDecoder: shared RGBA create failed — disabling zero-copy");
                    interopActive_ = false;
                }
            }
            // Lazily build the convert pass (shaders/sampler/raster/cbuffer), once.
            if (interopActive_ && !impl_->convVS) {
                if (!CompileConvertShaders(impl_->d3dDevice, &impl_->convVS, &impl_->convPS)) {
                    LOG_WARN("VideoDecoder: convert-shader build failed — disabling zero-copy");
                    interopActive_ = false;
                } else {
                    D3D11_SAMPLER_DESC sd = {};
                    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                    sd.MaxLOD = D3D11_FLOAT32_MAX;
                    impl_->d3dDevice->CreateSamplerState(&sd, &impl_->convSampler);
                    D3D11_RASTERIZER_DESC rd = {};
                    rd.FillMode = D3D11_FILL_SOLID;
                    rd.CullMode = D3D11_CULL_NONE;
                    impl_->d3dDevice->CreateRasterizerState(&rd, &impl_->convRaster);
                    D3D11_BUFFER_DESC bd = {};
                    bd.ByteWidth = 16;  // { float fullRange; float3 pad; }
                    bd.Usage = D3D11_USAGE_DEFAULT;
                    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    impl_->d3dDevice->CreateBuffer(&bd, nullptr, &impl_->convCB);
                }
            }
            // (Re)create the SRV-able NV12 intermediate + its plane SRVs on size change.
            if (interopActive_ && (impl_->convW != f->width || impl_->convH != f->height)) {
                if (impl_->srvUV) { impl_->srvUV->Release(); impl_->srvUV = nullptr; }
                if (impl_->srvY) { impl_->srvY->Release(); impl_->srvY = nullptr; }
                if (impl_->nv12Intermediate) { impl_->nv12Intermediate->Release(); impl_->nv12Intermediate = nullptr; }
                D3D11_TEXTURE2D_DESC nd = {};
                nd.Width = (UINT)f->width; nd.Height = (UINT)f->height;
                nd.MipLevels = 1; nd.ArraySize = 1; nd.Format = DXGI_FORMAT_NV12;
                nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT;
                nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                bool ok = SUCCEEDED(impl_->d3dDevice->CreateTexture2D(&nd, nullptr, &impl_->nv12Intermediate));
                if (ok) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC yv = {};
                    yv.Format = DXGI_FORMAT_R8_UNORM;  // plane 0 (Y)
                    yv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    yv.Texture2D.MipLevels = 1;
                    ok = SUCCEEDED(impl_->d3dDevice->CreateShaderResourceView(
                             impl_->nv12Intermediate, &yv, &impl_->srvY));
                    D3D11_SHADER_RESOURCE_VIEW_DESC uv = yv;
                    uv.Format = DXGI_FORMAT_R8G8_UNORM;  // plane 1 (interleaved UV, half-res)
                    ok = ok && SUCCEEDED(impl_->d3dDevice->CreateShaderResourceView(
                                   impl_->nv12Intermediate, &uv, &impl_->srvUV));
                }
                if (ok) { impl_->convW = f->width; impl_->convH = f->height; }
                else {
                    LOG_WARN("VideoDecoder: NV12 intermediate/SRV create failed — disabling zero-copy");
                    interopActive_ = false;
                }
            }
            if (interopActive_ && dst.gpuSharedTexture) {
                auto* decoderTex = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
                const UINT slice = (UINT)(intptr_t)f->data[1];
                auto* rgba = reinterpret_cast<ID3D11Texture2D*>(dst.gpuSharedTexture);
                ID3D11DeviceContext* ctx = impl_->d3dContext;
                D3D11_BOX box = {0, 0, 0, (UINT)f->width, (UINT)f->height, 1};
                impl_->d3dHwctx->lock(impl_->d3dHwctx->lock_ctx);
                if (!impl_->copyDoneQuery) {
                    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
                    impl_->d3dDevice->CreateQuery(&qd, &impl_->copyDoneQuery);
                }
                // 1) Decoder slice -> SRV-able NV12 intermediate.
                ctx->CopySubresourceRegion(impl_->nv12Intermediate, 0, 0, 0, 0, decoderTex, slice, &box);
                // 2) Fullscreen NV12->RGB draw into the shared RGBA texture.
                ID3D11RenderTargetView* rtv = nullptr;
                if (SUCCEEDED(impl_->d3dDevice->CreateRenderTargetView(rgba, nullptr, &rtv))) {
                    const float full = (f->color_range == AVCOL_RANGE_JPEG) ? 1.0f : 0.0f;
                    float cb[4] = {full, 0, 0, 0};
                    ctx->UpdateSubresource(impl_->convCB, 0, nullptr, cb, 0, 0);
                    D3D11_VIEWPORT vp = {0, 0, (float)f->width, (float)f->height, 0, 1};
                    ID3D11ShaderResourceView* srvs[2] = {impl_->srvY, impl_->srvUV};
                    ctx->IASetInputLayout(nullptr);
                    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx->VSSetShader(impl_->convVS, nullptr, 0);
                    ctx->PSSetShader(impl_->convPS, nullptr, 0);
                    ctx->PSSetShaderResources(0, 2, srvs);
                    ctx->PSSetSamplers(0, 1, &impl_->convSampler);
                    ctx->PSSetConstantBuffers(0, 1, &impl_->convCB);
                    ctx->RSSetState(impl_->convRaster);
                    ctx->RSSetViewports(1, &vp);
                    ctx->OMSetRenderTargets(1, &rtv, nullptr);
                    ctx->Draw(3, 0);
                    // Unbind so the next decode on this shared context isn't holding our views.
                    ID3D11ShaderResourceView* nullSrv[2] = {nullptr, nullptr};
                    ID3D11RenderTargetView* nullRtv = nullptr;
                    ctx->PSSetShaderResources(0, 2, nullSrv);
                    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
                    rtv->Release();
                }
                if (impl_->copyDoneQuery) ctx->End(impl_->copyDoneQuery);
                ctx->Flush();
                // Block until the GPU finishes, so the published frame's memory is coherent for
                // the Vulkan sample (which happens later, on the render thread).
                if (impl_->copyDoneQuery) {
                    BOOL done = FALSE;
                    for (int i = 0; i < 2000; ++i) {  // ~100ms cap; a wedged GPU can't hang us
                        if (ctx->GetData(impl_->copyDoneQuery, &done, sizeof(done), 0) == S_OK)
                            break;
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                }
                impl_->d3dHwctx->unlock(impl_->d3dHwctx->lock_ctx);

                dst.width = f->width;
                dst.height = f->height;
                dst.format = PixFormat::RGBA;  // convert already did YUV->RGB + range expand
                dst.fullRange = false;
                dst.gpu = true;
                const int64_t ticks = (f->best_effort_timestamp != AV_NOPTS_VALUE)
                                          ? f->best_effort_timestamp
                                          : f->pts;
                return (ticks != AV_NOPTS_VALUE) ? (double)ticks * timeBase : -1.0;
            }
        }
#endif
        AVFrame* src = f;
        if (impl_->hwPixFmt != AV_PIX_FMT_NONE && f->format == impl_->hwPixFmt) {
            if (!impl_->swFrame) impl_->swFrame = av_frame_alloc();
            av_frame_unref(impl_->swFrame);
            if (av_hwframe_transfer_data(impl_->swFrame, f, 0) < 0) {
                LOG_ERROR("VideoDecoder: hw frame download failed; dropping frame");
                return -2.0;
            }
            impl_->swFrame->pts = f->pts;
            impl_->swFrame->best_effort_timestamp = f->best_effort_timestamp;
            impl_->swFrame->color_range = f->color_range;
            src = impl_->swFrame;
        }

        const AVPixelFormat sf = (AVPixelFormat)src->format;
        AVFrame* yuv = src;
        PixFormat outFmt;
        if (sf == AV_PIX_FMT_YUV420P || sf == AV_PIX_FMT_YUVJ420P) {
            outFmt = PixFormat::I420;
        } else if (sf == AV_PIX_FMT_NV12) {
            outFmt = PixFormat::NV12;
        } else {
            // Rare formats -> I420 (no scale) so the GPU path stays uniform.
            if (!impl_->convFrame) impl_->convFrame = av_frame_alloc();
            if (!impl_->sws || impl_->swsW != src->width || impl_->swsH != src->height ||
                impl_->swsFmt != sf) {
                if (impl_->sws) sws_freeContext(impl_->sws);
                impl_->sws = sws_getContext(src->width, src->height, sf, src->width, src->height,
                                            AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr,
                                            nullptr);
                impl_->swsW = src->width;
                impl_->swsH = src->height;
                impl_->swsFmt = sf;
                av_frame_unref(impl_->convFrame);
                impl_->convFrame->format = AV_PIX_FMT_YUV420P;
                impl_->convFrame->width = src->width;
                impl_->convFrame->height = src->height;
                av_frame_get_buffer(impl_->convFrame, 32);
            }
            sws_scale(impl_->sws, src->data, src->linesize, 0, src->height, impl_->convFrame->data,
                      impl_->convFrame->linesize);
            yuv = impl_->convFrame;
            outFmt = PixFormat::I420;
        }

        FrameRing::Frame& dst = ring_.WriteBuffer();
        dst.gpu = false;  // CPU-plane frame (clears any prior zero-copy state on this slot)
        dst.width = yuv->width;
        dst.height = yuv->height;
        dst.format = outFmt;
        dst.fullRange = (src->color_range == AVCOL_RANGE_JPEG) || sf == AV_PIX_FMT_YUVJ420P;
        const int cw = (yuv->width + 1) / 2, ch = (yuv->height + 1) / 2;
        copyPlane(dst.plane[0], yuv->data[0], yuv->linesize[0], yuv->width, yuv->height);
        if (outFmt == PixFormat::I420) {
            copyPlane(dst.plane[1], yuv->data[1], yuv->linesize[1], cw, ch);
            copyPlane(dst.plane[2], yuv->data[2], yuv->linesize[2], cw, ch);
        } else {  // NV12: a single interleaved chroma plane, cw*2 bytes per row
            copyPlane(dst.plane[1], yuv->data[1], yuv->linesize[1], cw * 2, ch);
            dst.plane[2].clear();
        }
        const int64_t ticks = (src->best_effort_timestamp != AV_NOPTS_VALUE)
                                  ? src->best_effort_timestamp : src->pts;
        return (ticks != AV_NOPTS_VALUE) ? (double)ticks * timeBase : -1.0;
    };

    if (live_) {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        LiveLoop([&](void* f) { return fillFrame(static_cast<AVFrame*>(f)); }, timeBase);
        return;
    }

    while (!stop_.load()) {
        // Seek (serviced even while paused). A decoder needs several packets after a
        // flush to emit a frame, so we DECODE FORWARD from the keyframe to the exact
        // target, then publish that one. This is the standard accurate-seek primitive;
        // responsiveness comes from coalescing (latest-wins) + decoding on this thread.
        //
        // Future (web-smooth scrubbing on long-GOP / 4K, where exact-during-drag can't
        // keep up): go adaptive — show the nearest keyframe (or a pre-generated low-res
        // thumbnail/storyboard track) during the drag, and resolve to the exact frame on
        // release. The keyframe-preview variant lived here briefly; see git history.
        const double sk = seekRequest_.exchange(-1.0);
        if (sk >= 0.0) {
            const bool preview = seekPreview_.load();  // drag -> nearest keyframe (fast)
            const auto seekT0 = clock::now();
            av_seek_frame(impl_->fmt, impl_->videoStream, (int64_t)(sk / timeBase),
                          AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(impl_->codec);
            firstPtsSec = -1.0;
            audioGiveUp = false;  // re-arm audio sync at the new position

            // Decode forward to the EXACT target frame and display only that one. We do
            // NOT bail to the keyframe or interrupt mid-decode for a newer scrub request:
            // an exact seek is cheap now (only the displayed frame is downloaded), and the
            // atomic seekRequest_ coalesces — each finished seek immediately re-targets the
            // latest cursor position. The net effect is frame-exact previews that track the
            // cursor at decode rate, instead of jumping between keyframes.
            bool reached = false, filled = false;
            double lastPts = sk;
            while (!stop_.load() && !reached) {
                if (av_read_frame(impl_->fmt, pkt) < 0) break;  // EOF before target
                if (pkt->stream_index != impl_->videoStream) { av_packet_unref(pkt); continue; }
                const int sent = avcodec_send_packet(impl_->codec, pkt);
                av_packet_unref(pkt);
                if (sent != 0) continue;
                while (!stop_.load() && avcodec_receive_frame(impl_->codec, frame) == 0) {
                    // Read the PTS off the raw decoded frame — cheap, no GPU download. Only
                    // the frame we actually display is downloaded/converted (fillFrame);
                    // decoding past intermediate frames is fast, downloading them is not.
                    const int64_t ticks = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                              ? frame->best_effort_timestamp : frame->pts;
                    const double pts = (ticks != AV_NOPTS_VALUE) ? (double)ticks * timeBase : -1.0;
                    // Preview: take the first frame (the keyframe) — no forward decode.
                    // Exact: decode forward until we reach the target frame.
                    if (preview || pts < 0.0 || pts + 1e-3 >= sk) {
                        const double fpts = fillFrame(frame);  // copy this one to the ring
                        if (fpts > -2.0) { filled = true; lastPts = (fpts >= 0.0) ? fpts : sk; }
                        reached = true;
                        break;
                    }
                    // earlier than target → discard cheaply (no copy), keep decoding
                }
            }
            // Always publish the frame we landed on — during a drag this is the preview
            // keyframe; when settled it's the exact target. The knob is held steady by
            // the UI until the decoded position catches up (no snap-back on release).
            if (filled) {
                positionSec_.store(lastPts);
                ring_.Publish();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    clock::now() - seekT0).count();
                LOG_DEBUG("VideoDecoder: seek %.2fs -> landed %.2fs in %lldms", sk, lastPts,
                          (long long)ms);
            }
            wallStart = clock::now();
            continue;
        }

        // Pause: hold here without consuming packets (but wake to service a seek), then
        // advance the wall clock by the paused duration so pacing resumes seamlessly.
        if (paused_.load()) {
            const auto pauseBegin = clock::now();
            while (paused_.load() && seekRequest_.load() < 0.0 && !stop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            wallStart += clock::now() - pauseBegin;
            continue;
        }

        int r = av_read_frame(impl_->fmt, pkt);
        if (r < 0) {
            if (loopEnabled_.load()) {
                // EOF (or error) -> loop back to the start.
                av_seek_frame(impl_->fmt, impl_->videoStream, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(impl_->codec);
                firstPtsSec = -1.0;
                audioGiveUp = false;  // re-arm audio sync after looping to the start
                wallStart = clock::now();
            } else {
                // Loop off: mark ended and pause, holding the last published frame. A
                // Seek() (e.g. resume-from-end, or slideshow advance) clears `ended_`.
                ended_.store(true);
                paused_.store(true);
            }
            continue;
        }
        if (pkt->stream_index != impl_->videoStream) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(impl_->codec, pkt) == 0) {
            while (!stop_.load() && avcodec_receive_frame(impl_->codec, frame) == 0) {
                const double ptsSec = fillFrame(frame);
                if (ptsSec <= -2.0) continue;  // dropped
                if (ptsSec >= 0.0) {
                    positionSec_.store(ptsSec);
                    // Present this frame in step with the audio master clock. Fall back to
                    // the wall clock only when there is genuinely no usable audio clock: no
                    // audio track, or audio has stopped/ended (`audioGiveUp` latch). We must
                    // NOT bail just because video is momentarily far ahead of audio — at
                    // startup the audio device warms up a beat after the video thread, so the
                    // audio clock lags for the first ~second; bailing there is exactly what
                    // let video run away at raw (faster-than-realtime) software-decode speed.
                    const double mc = (masterClock_ && !audioGiveUp) ? masterClock_() : -1.0;
                    bool wall = (mc < 0.0);
                    if (!wall) {
                        // Wait until the audio clock reaches this PTS. Keep waiting as long as
                        // audio is still making progress; only give up (and hand pacing to the
                        // wall clock for the rest of this run) if audio stops advancing for a
                        // grace period — audio shorter than the video, or the device dropping.
                        double lastNow = mc;
                        int stalledMs = 0;
                        for (;;) {
                            if (stop_.load() || paused_.load() || seekRequest_.load() >= 0.0) break;
                            const double now = masterClock_();
                            if (now < 0.0) { wall = true; audioGiveUp = true; break; }  // audio gone
                            if (now >= ptsSec - 0.005) break;                            // audio reached it
                            if (now > lastNow + 1e-4) { lastNow = now; stalledMs = 0; }   // still advancing
                            else if ((stalledMs += 2) >= 1000) {                          // stalled -> wall
                                wall = true; audioGiveUp = true; break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                        if (wall) { firstPtsSec = ptsSec; wallStart = clock::now(); }  // re-anchor on handoff
                    }
                    if (wall) {
                        if (firstPtsSec < 0.0) { firstPtsSec = ptsSec; wallStart = clock::now(); }
                        auto target = wallStart + std::chrono::duration_cast<clock::duration>(
                                                      std::chrono::duration<double>(ptsSec - firstPtsSec));
                        std::this_thread::sleep_until(target);
                    }
                }
                ring_.Publish();
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

void VideoDecoder::Seek(double seconds, bool preview) {
    if (live_) return;  // a live stream has no timeline (#93)
    if (seconds < 0.0) seconds = 0.0;
    // Leave a little headroom before EOF so the forward-decode always finds a frame at
    // or after the target (otherwise a seek to the very end lands nothing to display).
    if (durationSec_ > 0.0 && seconds > durationSec_ - 0.1) seconds = durationSec_ - 0.1;
    if (seconds < 0.0) seconds = 0.0;
    LOG_DEBUG("VideoDecoder: seek request -> %.2fs (paused=%d)", seconds, (int)paused_.load());
    ended_.store(false);   // a seek leaves the EOF/hold state
    seekPreview_.store(preview);
    seekRequest_.store(seconds);
}

void VideoDecoder::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    open_ = false;
#if defined(_WIN32)
    // Release the per-slot shared NV12 textures + handles (zero-copy, #28). Safe after the
    // decode thread has joined; the renderer's imports keyed off these handles are torn
    // down separately when media changes.
    for (int i = 0; i < FrameRing::kBufferCount; ++i) {
        FrameRing::Frame& b = ring_.BufferSlot(i);
        if (b.gpuSharedTexture) {
            reinterpret_cast<ID3D11Texture2D*>(b.gpuSharedTexture)->Release();
            b.gpuSharedTexture = nullptr;
        }
        b.gpuSharedHandle = nullptr;  // KMT handle owned by the texture (freed by Release above)
        b.gpu = false;
    }
#endif
    impl_.reset();
    live_ = false;
    streamState_.store(StreamState::Idle);
    ioDeadlineNs_.store(0);
}

// ---- Live network stream (#93) --------------------------------------------------------

namespace {
int64_t SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string AvErrorText(int err) {
    // Socket errnos have no strerror text on MSVC ("Error number -138 occurred").
    if (err == AVERROR(ECONNREFUSED)) return "connection refused";
    if (err == AVERROR(ETIMEDOUT)) return "connection timed out";
    if (err == AVERROR(ECONNRESET)) return "connection reset";
    if (err == AVERROR(EHOSTUNREACH) || err == AVERROR(ENETUNREACH)) return "host unreachable";
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

// Errors a retry cannot fix: the build lacks the protocol/demuxer/decoder, or the
// server said no.
bool IsFatalOpenError(int err) {
    return err == AVERROR_PROTOCOL_NOT_FOUND || err == AVERROR_DEMUXER_NOT_FOUND ||
           err == AVERROR_DECODER_NOT_FOUND || err == AVERROR_STREAM_NOT_FOUND ||
           err == AVERROR_HTTP_BAD_REQUEST || err == AVERROR_HTTP_UNAUTHORIZED ||
           err == AVERROR_HTTP_FORBIDDEN || err == AVERROR_HTTP_NOT_FOUND;
}

std::string LowerScheme(const std::string& url) {
    const size_t p = url.find("://");
    std::string s = (p == std::string::npos) ? std::string() : url.substr(0, p);
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool IsHls(const std::string& url) {
    std::string path = url.substr(0, url.find_first_of("?#"));
    if (path.size() < 5) return false;
    std::string ext = path.substr(path.size() - 5);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".m3u8";
}

constexpr int64_t kSec = 1000000000LL;
}  // namespace

const char* VideoDecoder::StreamStateName(StreamState s) {
    switch (s) {
        case StreamState::Idle: return "idle";
        case StreamState::Connecting: return "connecting";
        case StreamState::Streaming: return "streaming";
        case StreamState::Reconnecting: return "reconnecting";
        case StreamState::Failed: return "failed";
    }
    return "idle";
}

int VideoDecoder::InterruptCallback(void* self) {
    auto* d = static_cast<VideoDecoder*>(self);
    if (d->stop_.load()) return 1;
    const int64_t dl = d->ioDeadlineNs_.load();
    return (dl != 0 && SteadyNowNs() > dl) ? 1 : 0;
}

VideoDecoder::StreamStats VideoDecoder::GetStreamStats() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return stats_;
}

bool VideoDecoder::OpenLive(const std::string& url) {
    Stop();
    static std::once_flag netInit;
    std::call_once(netInit, [] { avformat_network_init(); });
    live_ = true;
    streamUrl_ = url;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        stats_ = StreamStats{};
    }
    durationSec_ = 0.0;
    frameRate_ = 0.0;
    width_ = height_ = 0;
    positionSec_.store(0.0);
    seekRequest_.store(-1.0);
    ended_.store(false);
    paused_.store(false);
    streamState_.store(StreamState::Connecting);
    open_ = true;
    stop_ = false;
    thread_ = std::thread(&VideoDecoder::DecodeLoop, this);
    return true;
}

bool VideoDecoder::OpenLiveInput(std::string& err, bool& fatal) {
    fatal = false;
    impl_ = std::make_unique<Impl>();
    hardware_ = false;
    hwName_ = "software";
#if defined(_WIN32)
    interopActive_ = false;
#endif
    impl_->fmt = avformat_alloc_context();
    impl_->fmt->interrupt_callback.callback = &VideoDecoder::InterruptCallback;
    impl_->fmt->interrupt_callback.opaque = this;

    // Low-latency demux. Unknown keys are simply left unconsumed by protocols/demuxers
    // that don't have them, so one dictionary serves every scheme.
    const std::string scheme = LowerScheme(streamUrl_);
    const bool hls = IsHls(streamUrl_);
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "max_delay", "500000", 0);           // demuxer reorder window, us
    if (!hls) {
        // Small, but enough to see an SPS/PPS + the first IDR of a 4K SBS stream.
        av_dict_set(&opts, "probesize", "1000000", 0);
        av_dict_set(&opts, "analyzeduration", "1500000", 0);
    }
    av_dict_set(&opts, "rw_timeout", "5000000", 0);          // any protocol: dead peer, us
    if (scheme == "rtsp" || scheme == "rtsps") {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);      // no UDP loss / NAT trouble
        av_dict_set(&opts, "reorder_queue_size", "16", 0);
        av_dict_set(&opts, "timeout", "5000000", 0);         // socket I/O timeout, us
    } else if (scheme == "udp" || scheme == "rtp") {
        av_dict_set(&opts, "timeout", "5000000", 0);         // raise an error after 5 s silence
        av_dict_set(&opts, "buffer_size", "16777216", 0);    // 4K I-frames arrive in bursts
        av_dict_set(&opts, "overrun_nonfatal", "1", 0);
        av_dict_set(&opts, "fifo_size", "1000000", 0);
    } else if (scheme == "http" || scheme == "https" || scheme == "tcp") {
        av_dict_set(&opts, "timeout", "5000000", 0);
        if (scheme != "tcp") {
            av_dict_set(&opts, "reconnect", "1", 0);
            av_dict_set(&opts, "reconnect_streamed", "1", 0);
            // NOT reconnect_on_network_error: it makes a refused connect (a typo'd URL,
            // a sender not up yet) retry inside the protocol until the 8 s budget, so a
            // dead URL took ~30 s to report. LiveLoop's own backoff handles that case.
            av_dict_set(&opts, "reconnect_delay_max", "2", 0);
        }
    }

    // Connect + probe must finish inside this budget (a dead server fails fast; Stop()
    // interrupts it at once through the callback).
    ioDeadlineNs_.store(SteadyNowNs() + (hls ? 15 : 8) * kSec);
    int rc = avformat_open_input(&impl_->fmt, streamUrl_.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        impl_->fmt = nullptr;  // avformat_open_input frees it on failure
        err = (rc == AVERROR_EXIT && !stop_.load()) ? "connect timed out" : AvErrorText(rc);
        fatal = IsFatalOpenError(rc);
        ioDeadlineNs_.store(0);
        return false;
    }
    rc = avformat_find_stream_info(impl_->fmt, nullptr);
    if (rc < 0) {
        err = (rc == AVERROR_EXIT) ? "no stream info (timed out)" : AvErrorText(rc);
        ioDeadlineNs_.store(0);
        return false;
    }
    ioDeadlineNs_.store(0);

#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVCodec* dec = nullptr;
#else
    AVCodec* dec = nullptr;
#endif
    impl_->videoStream = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (impl_->videoStream < 0 || !dec) {
        err = (impl_->videoStream == AVERROR_DECODER_NOT_FOUND) ? "no decoder for the video codec"
                                                                 : "no video stream";
        fatal = true;
        return false;
    }
    // Audio / data streams are dropped at the demuxer (v1: no audio for streams).
    for (unsigned i = 0; i < impl_->fmt->nb_streams; ++i)
        if ((int)i != impl_->videoStream) impl_->fmt->streams[i]->discard = AVDISCARD_ALL;
    codecName_ = dec->name;
    if (!OpenVideoCodec(dec, streamUrl_)) {
        err = "cannot open the video decoder";
        fatal = true;
        return false;
    }
    AVStream* st = impl_->fmt->streams[impl_->videoStream];
    frameRate_ = av_q2d(st->avg_frame_rate);
    if (frameRate_ <= 0.0) frameRate_ = av_q2d(st->r_frame_rate);
    return true;
}

void VideoDecoder::LiveLoop(const std::function<double(void*)>& fill, double& timeBase) {
    using clock = std::chrono::steady_clock;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const bool hls = IsHls(streamUrl_);
    const int64_t readTimeoutNs = (hls ? 15 : 5) * kSec;

    // Before the first successful stream a dead URL gives up after kMaxInitialFailures
    // (1 + 2 + 4 s of backoff); once it has streamed, it retries forever (the sender
    // may come back) with backoff capped at 8 s.
    constexpr int kMaxInitialFailures = 3;
    int initialFailures = 0;
    bool everStreamed = false;
    int backoffSec = 1;

    auto setError = [&](const std::string& e) {
        std::lock_guard<std::mutex> lk(statsMutex_);
        stats_.lastError = e;
    };

    while (!stop_.load()) {
        const auto t0 = clock::now();
        std::string err;
        bool fatal = false;
        if (OpenLiveInput(err, fatal)) {
            timeBase = av_q2d(impl_->fmt->streams[impl_->videoStream]->time_base);
            LOG_INFO("VideoDecoder: stream opened in %.0f ms (codec=%s backend=%s)",
                     std::chrono::duration<double, std::milli>(clock::now() - t0).count(),
                     codecName_, hwName_);
            {
                std::lock_guard<std::mutex> lk(statsMutex_);
                stats_.codec = codecName_;
                stats_.backend = hwName_;
            }
            bool gotFrame = false;
            auto winStart = clock::now();
            uint64_t winFrames = 0, winBytes = 0;
            for (;;) {
                if (stop_.load()) break;
                ioDeadlineNs_.store(SteadyNowNs() + readTimeoutNs);
                const int r = av_read_frame(impl_->fmt, pkt);
                if (r < 0) {
                    if (stop_.load()) break;
                    err = (r == AVERROR_EOF) ? "stream ended"
                          : (r == AVERROR_EXIT) ? "no data for 5 s"
                                                : AvErrorText(r);
                    break;
                }
                winBytes += (uint64_t)pkt->size;
                if (pkt->stream_index == impl_->videoStream &&
                    avcodec_send_packet(impl_->codec, pkt) == 0) {
                    while (!stop_.load() && avcodec_receive_frame(impl_->codec, frame) == 0) {
                        const double pts = fill(frame);
                        if (pts <= -2.0) continue;  // hw download failed: dropped
                        if (pts >= 0.0) positionSec_.store(pts);
                        const bool overwrote = ring_.Publish();
                        ++winFrames;
                        std::lock_guard<std::mutex> lk(statsMutex_);
                        ++stats_.frames;
                        if (overwrote) ++stats_.dropped;
                        stats_.width = frame->width;
                        stats_.height = frame->height;
                        stats_.backend = hwName_;  // PickHwFormat may fall back per frame
                        if (!gotFrame) {
                            gotFrame = true;
                            stats_.connectMs =
                                std::chrono::duration<double, std::milli>(clock::now() - t0)
                                    .count();
                            stats_.lastError.clear();
                            if (everStreamed) ++stats_.reconnects;
                            everStreamed = true;
                            backoffSec = 1;
                            width_ = frame->width;
                            height_ = frame->height;
                            streamState_.store(StreamState::Streaming);
                        }
                    }
                }
                av_packet_unref(pkt);
                const double winSec =
                    std::chrono::duration<double>(clock::now() - winStart).count();
                if (winSec >= 1.0) {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.fpsIn = (float)(winFrames / winSec);
                    stats_.kbps = (float)(winBytes * 8.0 / 1000.0 / winSec);
                    winStart = clock::now();
                    winFrames = winBytes = 0;
                }
            }
            ioDeadlineNs_.store(0);
        }
        impl_.reset();  // close the input + decoder; the next attempt starts clean
        if (stop_.load()) break;

        LOG_WARN("VideoDecoder: stream %s: %s", everStreamed ? "lost" : "connect failed",
                 err.c_str());
        setError(err);
        {
            std::lock_guard<std::mutex> lk(statsMutex_);
            stats_.fpsIn = 0.0f;
            stats_.kbps = 0.0f;
        }
        if (fatal || (!everStreamed && ++initialFailures >= kMaxInitialFailures)) {
            streamState_.store(StreamState::Failed);
            break;
        }
        streamState_.store(everStreamed ? StreamState::Reconnecting : StreamState::Connecting);
        // Interruptible backoff sleep.
        const auto until = clock::now() + std::chrono::seconds(backoffSec);
        while (!stop_.load() && clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        backoffSec = (std::min)(backoffSec * 2, 8);  // parenthesised: windows.h min()
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

#else  // !MEDIAPLAYER_WITH_FFMPEG

struct VideoDecoder::Impl {};
VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() {}
bool VideoDecoder::Open(const std::string&) {
    LOG_WARN("VideoDecoder: built without FFmpeg — video unavailable");
    return false;
}
void VideoDecoder::DecodeLoop() {}
void VideoDecoder::Seek(double, bool) {}
void VideoDecoder::Stop() {}
bool VideoDecoder::OpenLive(const std::string&) {
    LOG_WARN("VideoDecoder: built without FFmpeg — streams unavailable");
    return false;
}
const char* VideoDecoder::StreamStateName(StreamState) { return "idle"; }
VideoDecoder::StreamStats VideoDecoder::GetStreamStats() const { return StreamStats{}; }
void VideoDecoder::LiveLoop(const std::function<double(void*)>&, double&) {}
bool VideoDecoder::OpenLiveInput(std::string&, bool&) { return false; }
bool VideoDecoder::OpenVideoCodec(const void*, const std::string&) { return false; }
int VideoDecoder::InterruptCallback(void*) { return 0; }

#endif

} // namespace mp
