#include <jni.h>
#include <windows.h>
#include <audioclient.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include "third_party/nvidia/nvEncodeAPI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "windowsapp.lib")

namespace {

constexpr uint32_t VIDEO_MAGIC = 0x56535357; // WSSV
constexpr uint32_t VIDEO_VERSION = 1;
constexpr uint32_t VIDEO_CODEC_H264_ANNEX_B = 1;
constexpr uint32_t VIDEO_FLAG_KEYFRAME = 1;
constexpr size_t VIDEO_HEADER_BYTES = 40;
constexpr uint32_t AUDIO_MAGIC = 0x41535357; // WSSA
constexpr uint32_t AUDIO_VERSION = 1;
constexpr uint32_t AUDIO_CODEC_PCM_FLOAT32 = 1;
constexpr size_t AUDIO_HEADER_BYTES = 40;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
constexpr DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x00000002;
#endif

JavaVM *g_vm = nullptr;
jobject g_bridge = nullptr;
jmethodID g_write = nullptr;
jmethodID g_flush = nullptr;
jmethodID g_status = nullptr;
std::thread g_worker;
std::atomic_bool g_running{false};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;
    ComPtr(ComPtr &&other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    ComPtr &operator=(ComPtr &&other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T **put() {
        reset();
        return &ptr_;
    }

    T *get() const { return ptr_; }
    T *operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void attach(T *ptr) {
        reset();
        ptr_ = ptr;
    }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T *ptr_ = nullptr;
};

struct CaptureConfig {
    int width;
    int height;
    int fps;
    int bitrate;
    int display_index;
    int adapter_index;
};

struct DuplicationContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
};

struct WgcContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Texture2D> latest;
    std::mutex texture_mutex;
    uint64_t captured_frames = 0;
    uint64_t last_reported_frame = 0;
    bool has_latest_frame = false;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived;
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    uint32_t source_width = 0;
    uint32_t source_height = 0;
};

struct FrameSendResult {
    bool ok = false;
    bool captured_new_frame = false;
    bool published_packet = false;
};

static void put_u32(std::vector<uint8_t> &buffer, size_t offset, uint32_t value) {
    buffer[offset] = static_cast<uint8_t>(value);
    buffer[offset + 1] = static_cast<uint8_t>(value >> 8);
    buffer[offset + 2] = static_cast<uint8_t>(value >> 16);
    buffer[offset + 3] = static_cast<uint8_t>(value >> 24);
}

static void put_u64(std::vector<uint8_t> &buffer, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        buffer[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

static uint64_t now_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void sleep_until_precise(std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;
    for (;;) {
        auto now = steady_clock::now();
        if (now >= deadline) {
            return;
        }
        auto remaining = duration_cast<microseconds>(deadline - now);
        if (remaining.count() <= 500) {
            while (steady_clock::now() < deadline) {
                YieldProcessor();
            }
            return;
        }

        static thread_local HANDLE timer = []() {
            HANDLE high_resolution = CreateWaitableTimerExW(
                    nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (high_resolution) {
                return high_resolution;
            }
            return CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }();
        if (!timer) {
            std::this_thread::sleep_for(remaining - microseconds(400));
            continue;
        }

        LARGE_INTEGER due_time{};
        due_time.QuadPart = -static_cast<LONGLONG>((remaining.count() - 300) * 10);
        if (SetWaitableTimer(timer, &due_time, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            std::this_thread::sleep_for(remaining - microseconds(400));
        }
    }
}

static std::string hr_hex(HRESULT hr) {
    std::ostringstream out;
    out << "0x" << std::hex << static_cast<uint32_t>(hr);
    return out.str();
}

static std::string nvenc_status(NVENCSTATUS status) {
    switch (status) {
        case NV_ENC_SUCCESS: return "NV_ENC_SUCCESS";
        case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NV_ENC_ERR_NO_ENCODE_DEVICE";
        case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
        case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
        case NV_ENC_ERR_INVALID_DEVICE: return "NV_ENC_ERR_INVALID_DEVICE";
        case NV_ENC_ERR_DEVICE_NOT_EXIST: return "NV_ENC_ERR_DEVICE_NOT_EXIST";
        case NV_ENC_ERR_INVALID_PTR: return "NV_ENC_ERR_INVALID_PTR";
        case NV_ENC_ERR_INVALID_PARAM: return "NV_ENC_ERR_INVALID_PARAM";
        case NV_ENC_ERR_INVALID_CALL: return "NV_ENC_ERR_INVALID_CALL";
        case NV_ENC_ERR_UNSUPPORTED_PARAM: return "NV_ENC_ERR_UNSUPPORTED_PARAM";
        case NV_ENC_ERR_INVALID_VERSION: return "NV_ENC_ERR_INVALID_VERSION";
        case NV_ENC_ERR_MAP_FAILED: return "NV_ENC_ERR_MAP_FAILED";
        case NV_ENC_ERR_RESOURCE_REGISTER_FAILED: return "NV_ENC_ERR_RESOURCE_REGISTER_FAILED";
        case NV_ENC_ERR_RESOURCE_NOT_REGISTERED: return "NV_ENC_ERR_RESOURCE_NOT_REGISTERED";
        case NV_ENC_ERR_RESOURCE_NOT_MAPPED: return "NV_ENC_ERR_RESOURCE_NOT_MAPPED";
        default: break;
    }
    std::ostringstream out;
    out << "NVENC status " << static_cast<int>(status);
    return out.str();
}

static void report_status(JNIEnv *env, const std::string &message) {
    if (!env || !g_bridge || !g_status) {
        return;
    }
    jstring text = env->NewStringUTF(message.c_str());
    if (!text) {
        return;
    }
    env->CallVoidMethod(g_bridge, g_status, text);
    env->DeleteLocalRef(text);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

static bool publish_native_packet(JNIEnv *env, const std::vector<uint8_t> &packet) {
    if (!env || !g_bridge || !g_write) {
        return false;
    }
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(packet.size()));
    if (!bytes) {
        return false;
    }
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(packet.size()), reinterpret_cast<const jbyte *>(packet.data()));
    env->CallVoidMethod(g_bridge, g_write, bytes, 0, static_cast<jint>(packet.size()));
    env->DeleteLocalRef(bytes);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return true;
}

static bool contains_h264_idr(const uint8_t *data, uint32_t size) {
    if (!data || size < 5) {
        return false;
    }
    for (uint32_t i = 0; i + 4 < size; ++i) {
        uint32_t start_code_size = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start_code_size = 3;
        } else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            start_code_size = 4;
        }
        if (start_code_size == 0) {
            continue;
        }
        uint32_t nal_offset = i + start_code_size;
        if (nal_offset < size && (data[nal_offset] & 0x1f) == 5) {
            return true;
        }
        i = nal_offset;
    }
    return false;
}

static void scale_bgra(
        const uint8_t *source,
        uint32_t source_width,
        uint32_t source_height,
        uint32_t source_stride,
        uint8_t *target,
        uint32_t target_width,
        uint32_t target_height,
        uint32_t target_stride) {
    for (uint32_t y = 0; y < target_height; ++y) {
        uint32_t sy = static_cast<uint64_t>(y) * source_height / target_height;
        const uint8_t *src_row = source + sy * source_stride;
        uint8_t *dst_row = target + y * target_stride;
        for (uint32_t x = 0; x < target_width; ++x) {
            uint32_t sx = static_cast<uint64_t>(x) * source_width / target_width;
            std::memcpy(dst_row + x * 4, src_row + sx * 4, 4);
        }
    }
}

static bool create_device_for_adapter(
        IDXGIAdapter *adapter,
        ComPtr<ID3D11Device> &device,
        ComPtr<ID3D11DeviceContext> &context,
        HRESULT &hr) {
    D3D_FEATURE_LEVEL feature_level{};
    hr = D3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            device.put(),
            &feature_level,
            context.put());
    return SUCCEEDED(hr);
}

static bool try_create_duplication_for_output(
        JNIEnv *env,
        IDXGIAdapter *adapter,
        IDXGIOutput *output,
        uint32_t adapter_index,
        uint32_t output_index,
        DuplicationContext &result) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr{};
    if (!create_device_for_adapter(adapter, device, context, hr)) {
        report_status(env, "adapter " + std::to_string(adapter_index) + " D3D11CreateDevice failed: " + hr_hex(hr));
        return false;
    }

    DXGI_OUTPUT_DESC output_desc{};
    output->GetDesc(&output_desc);
    uint32_t source_width = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
    uint32_t source_height = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;

    ComPtr<IDXGIOutput1> output1;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(output1.put()));
    if (FAILED(hr)) {
        report_status(env, "adapter " + std::to_string(adapter_index) + " output " + std::to_string(output_index)
                           + " IDXGIOutput1 query failed: " + hr_hex(hr));
        return false;
    }

    ComPtr<IDXGIOutputDuplication> duplication;
    hr = output1->DuplicateOutput(device.get(), duplication.put());
    if (FAILED(hr)) {
        report_status(env, "adapter " + std::to_string(adapter_index) + " output " + std::to_string(output_index)
                           + " DuplicateOutput failed: " + hr_hex(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = source_width;
    staging_desc.Height = source_height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    hr = device->CreateTexture2D(&staging_desc, nullptr, staging.put());
    if (FAILED(hr)) {
        report_status(env, "adapter " + std::to_string(adapter_index) + " output " + std::to_string(output_index)
                           + " CreateTexture2D staging failed: " + hr_hex(hr));
        return false;
    }

    result.device = std::move(device);
    result.context = std::move(context);
    result.duplication = std::move(duplication);
    result.staging = std::move(staging);
    result.source_width = source_width;
    result.source_height = source_height;
    report_status(env, "DXGI duplication started on adapter " + std::to_string(adapter_index)
                       + " output " + std::to_string(output_index)
                       + " (" + std::to_string(source_width) + "x" + std::to_string(source_height) + ")");
    return true;
}

static bool open_first_working_duplication(JNIEnv *env, int display_index, int requested_adapter_index,
                                           DuplicationContext &result) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory.put()));
    if (FAILED(hr)) {
        report_status(env, "CreateDXGIFactory1 failed: " + hr_hex(hr));
        return false;
    }

    uint32_t output_ordinal = 0;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter> adapter;
        hr = factory->EnumAdapters(adapter_index, adapter.put());
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            report_status(env, "EnumAdapters(" + std::to_string(adapter_index) + ") failed: " + hr_hex(hr));
            continue;
        }

        DXGI_ADAPTER_DESC adapter_desc{};
        adapter->GetDesc(&adapter_desc);
        report_status(env, "checking adapter " + std::to_string(adapter_index));
        if (requested_adapter_index >= 0 && static_cast<int>(adapter_index) != requested_adapter_index) {
            continue;
        }

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(output_index, output.put());
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                report_status(env, "EnumOutputs(" + std::to_string(output_index) + ") failed: " + hr_hex(hr));
                continue;
            }
            if (static_cast<int>(output_ordinal) != display_index) {
                ++output_ordinal;
                continue;
            }
            if (try_create_duplication_for_output(env, adapter.get(), output.get(), adapter_index, output_index, result)) {
                return true;
            }
            ++output_ordinal;
        }
    }
    return false;
}

static bool create_default_device(int requested_adapter_index, ComPtr<ID3D11Device> &device,
                                  ComPtr<ID3D11DeviceContext> &context, HRESULT &hr) {
    ComPtr<IDXGIFactory1> factory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory.put()));
    if (SUCCEEDED(hr)) {
        if (requested_adapter_index >= 0) {
            ComPtr<IDXGIAdapter> adapter;
            hr = factory->EnumAdapters(static_cast<UINT>(requested_adapter_index), adapter.put());
            if (SUCCEEDED(hr) && adapter && create_device_for_adapter(adapter.get(), device, context, hr)) {
                return true;
            }
        }
        for (UINT adapter_index = 0;; ++adapter_index) {
            ComPtr<IDXGIAdapter> adapter;
            hr = factory->EnumAdapters(adapter_index, adapter.put());
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                continue;
            }
            DXGI_ADAPTER_DESC desc{};
            adapter->GetDesc(&desc);
            if (desc.VendorId != 0x10DE) {
                continue;
            }
            if (create_device_for_adapter(adapter.get(), device, context, hr)) {
                return true;
            }
        }
    }

    D3D_FEATURE_LEVEL feature_level{};
    hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            device.put(),
            &feature_level,
            context.put());
    return SUCCEEDED(hr);
}

class H264Encoder {
public:
    H264Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate)
            : width_(width),
              height_(height),
              fps_(fps > 0 ? fps : 60),
              bitrate_(bitrate > 0 ? bitrate : 15'000'000),
              frame_duration_(10'000'000LL / static_cast<LONGLONG>(fps_)) {
    }

    bool open(JNIEnv *env) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            report_status(env, "MFStartup failed: " + hr_hex(hr));
            return false;
        }
        media_foundation_started_ = true;

        if (create_hardware_transform(env)) {
            if (!configure(env)) {
                report_status(env, "hardware H.264 encoder rejected CPU NV12 input, trying system encoder");
                transform_.reset();
            }
        }
        if (!transform_) {
            if (!create_system_transform(env)) {
                report_status(env, "no H.264 encoder MFT available");
                return false;
            }
            if (!configure(env)) {
                return false;
            }
        }

        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        report_status(env, "H.264 encoder ready (" + std::to_string(width_) + "x" + std::to_string(height_)
                           + " @" + std::to_string(fps_) + "fps, "
                           + std::to_string(bitrate_ / 1'000'000) + "Mbps)");
        return true;
    }

    bool encode(JNIEnv *env, const uint8_t *nv12, uint32_t nv12_size) {
        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateSample(sample.put());
        if (FAILED(hr)) {
            report_status(env, "MFCreateSample failed: " + hr_hex(hr));
            return false;
        }

        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(nv12_size, buffer.put());
        if (FAILED(hr)) {
            report_status(env, "MFCreateMemoryBuffer failed: " + hr_hex(hr));
            return false;
        }

        BYTE *target = nullptr;
        DWORD capacity = 0;
        DWORD current = 0;
        hr = buffer->Lock(&target, &capacity, &current);
        if (FAILED(hr)) {
            report_status(env, "encoder input buffer lock failed: " + hr_hex(hr));
            return false;
        }
        std::memcpy(target, nv12, nv12_size);
        buffer->Unlock();
        buffer->SetCurrentLength(nv12_size);

        sample->AddBuffer(buffer.get());
        sample->SetSampleTime(frame_index_ * frame_duration_);
        sample->SetSampleDuration(frame_duration_);

        hr = transform_->ProcessInput(0, sample.get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            return drain(env);
        }
        if (FAILED(hr)) {
            report_status(env, "encoder ProcessInput failed: " + hr_hex(hr));
            return false;
        }
        ++frame_index_;
        return drain(env);
    }

    ~H264Encoder() {
        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        if (media_foundation_started_) {
            MFShutdown();
        }
    }

private:
    bool create_hardware_transform(JNIEnv *env) {
        IMFActivate **activates = nullptr;
        UINT32 count = 0;
        MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
        HRESULT hr = MFTEnumEx(
                MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                nullptr,
                &output_info,
                &activates,
                &count);
        if (FAILED(hr) || count == 0) {
            report_status(env, "hardware H.264 encoder MFT not found: " + hr_hex(hr));
            return false;
        }

        bool created = false;
        for (UINT32 i = 0; i < count && !created; ++i) {
            IMFTransform *raw = nullptr;
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(&raw));
            if (SUCCEEDED(hr) && raw) {
                transform_.attach(raw);
                created = true;
                report_status(env, "hardware H.264 encoder MFT selected");
            }
        }
        for (UINT32 i = 0; i < count; ++i) {
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
        return created;
    }

    bool create_system_transform(JNIEnv *env) {
        ComPtr<IMFTransform> transform;
        HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(transform.put()));
        if (FAILED(hr)) {
            report_status(env, "system H.264 encoder MFT failed: " + hr_hex(hr));
            return false;
        }
        transform_ = std::move(transform);
        report_status(env, "system H.264 encoder MFT selected");
        return true;
    }

    bool configure(JNIEnv *env) {
        configure_codec_api(env);

        ComPtr<IMFMediaType> output_type;
        HRESULT hr = MFCreateMediaType(output_type.put());
        if (FAILED(hr)) {
            report_status(env, "MFCreateMediaType output failed: " + hr_hex(hr));
            return false;
        }
        output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_);
        output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(output_type.get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(output_type.get(), MF_MT_FRAME_RATE, fps_, 1);
        MFSetAttributeRatio(output_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);

        hr = transform_->SetOutputType(0, output_type.get(), 0);
        if (FAILED(hr)) {
            report_status(env, "encoder SetOutputType failed: " + hr_hex(hr));
            return false;
        }

        ComPtr<IMFMediaType> input_type;
        hr = MFCreateMediaType(input_type.put());
        if (FAILED(hr)) {
            report_status(env, "MFCreateMediaType input failed: " + hr_hex(hr));
            return false;
        }
        input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(input_type.get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(input_type.get(), MF_MT_FRAME_RATE, fps_, 1);
        MFSetAttributeRatio(input_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = transform_->SetInputType(0, input_type.get(), 0);
        if (FAILED(hr)) {
            report_status(env, "encoder SetInputType failed: " + hr_hex(hr));
            return false;
        }

        MFT_OUTPUT_STREAM_INFO info{};
        hr = transform_->GetOutputStreamInfo(0, &info);
        output_buffer_size_ = SUCCEEDED(hr) && info.cbSize > 0 ? info.cbSize : width_ * height_;
        return true;
    }

    void configure_codec_api(JNIEnv *env) {
        ComPtr<ICodecAPI> codec_api;
        HRESULT hr = transform_->QueryInterface(IID_PPV_ARGS(codec_api.put()));
        if (FAILED(hr)) {
            return;
        }

        VARIANT value{};
        value.vt = VT_UI4;
        value.ulVal = eAVEncCommonRateControlMode_CBR;
        codec_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &value);
        value.ulVal = bitrate_;
        codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
        value.ulVal = fps_;
        codec_api->SetValue(&CODECAPI_AVEncMPVGOPSize, &value);
        value.ulVal = 80;
        codec_api->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &value);
        report_status(env, "encoder rate control configured");
    }

    bool drain(JNIEnv *env) {
        bool ok = true;
        while (true) {
            ComPtr<IMFSample> output_sample;
            HRESULT hr = MFCreateSample(output_sample.put());
            if (FAILED(hr)) {
                report_status(env, "MFCreateSample output failed: " + hr_hex(hr));
                return false;
            }

            ComPtr<IMFMediaBuffer> output_buffer;
            hr = MFCreateMemoryBuffer(output_buffer_size_, output_buffer.put());
            if (FAILED(hr)) {
                report_status(env, "MFCreateMemoryBuffer output failed: " + hr_hex(hr));
                return false;
            }
            output_sample->AddBuffer(output_buffer.get());

            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = 0;
            output.pSample = output_sample.get();
            DWORD status = 0;
            hr = transform_->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) {
                output.pEvents->Release();
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return ok;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                report_status(env, "encoder stream changed");
                return true;
            }
            if (FAILED(hr)) {
                report_status(env, "encoder ProcessOutput failed: " + hr_hex(hr));
                return false;
            }

            DWORD total_length = 0;
            hr = output_sample->ConvertToContiguousBuffer(output_buffer.put());
            if (FAILED(hr)) {
                report_status(env, "encoder contiguous output failed: " + hr_hex(hr));
                return false;
            }
            hr = output_buffer->GetCurrentLength(&total_length);
            if (FAILED(hr) || total_length == 0) {
                continue;
            }

            BYTE *payload = nullptr;
            DWORD max_length = 0;
            DWORD current_length = 0;
            hr = output_buffer->Lock(&payload, &max_length, &current_length);
            if (FAILED(hr)) {
                report_status(env, "encoder output lock failed: " + hr_hex(hr));
                return false;
            }

            UINT32 clean_point = 0;
            output_sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);
            ok = send_packet(env, payload, current_length, clean_point != 0 || contains_h264_idr(payload, current_length)) && ok;
            output_buffer->Unlock();
        }
    }

    bool send_packet(JNIEnv *env, const uint8_t *payload, uint32_t payload_size, bool keyframe) {
        std::vector<uint8_t> packet(VIDEO_HEADER_BYTES + payload_size);
        put_u32(packet, 0, VIDEO_MAGIC);
        put_u32(packet, 4, VIDEO_VERSION);
        put_u32(packet, 8, VIDEO_CODEC_H264_ANNEX_B);
        put_u32(packet, 12, keyframe ? VIDEO_FLAG_KEYFRAME : 0);
        put_u32(packet, 16, width_);
        put_u32(packet, 20, height_);
        put_u64(packet, 24, static_cast<uint64_t>((frame_index_ > 0 ? frame_index_ - 1 : 0) * frame_duration_ / 10));
        put_u32(packet, 32, payload_size);
        put_u32(packet, 36, 0);
        std::memcpy(packet.data() + VIDEO_HEADER_BYTES, payload, payload_size);

        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(packet.size()));
        if (!bytes) {
            return false;
        }
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(packet.size()), reinterpret_cast<const jbyte *>(packet.data()));
        env->CallVoidMethod(g_bridge, g_write, bytes, 0, static_cast<jint>(packet.size()));
        env->DeleteLocalRef(bytes);
        return !env->ExceptionCheck();
    }

    uint32_t width_;
    uint32_t height_;
    uint32_t fps_;
    uint32_t bitrate_;
    LONGLONG frame_duration_;
    LONGLONG frame_index_ = 0;
    DWORD output_buffer_size_ = 0;
    bool media_foundation_started_ = false;
    ComPtr<IMFTransform> transform_;
};

class NvencTextureEncoder {
public:
    NvencTextureEncoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate)
            : width_(width),
              height_(height),
              fps_(fps > 0 ? fps : 60),
              bitrate_(bitrate > 0 ? bitrate : 15'000'000) {
    }

    bool open(JNIEnv *env, ID3D11Device *device) {
        if (!device) {
            return false;
        }

        module_ = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!module_) {
            report_status(env, "NVENC driver API not found");
            return false;
        }

        auto create_instance = reinterpret_cast<NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *)>(
                GetProcAddress(module_, "NvEncodeAPICreateInstance"));
        auto max_supported_version = reinterpret_cast<NVENCSTATUS(NVENCAPI *)(uint32_t *)>(
                GetProcAddress(module_, "NvEncodeAPIGetMaxSupportedVersion"));
        if (!create_instance) {
            report_status(env, "NvEncodeAPICreateInstance not exported");
            return false;
        }
        uint32_t api_version = NVENCAPI_VERSION;
        if (max_supported_version) {
            uint32_t driver_version = 0;
            NVENCSTATUS version_status = max_supported_version(&driver_version);
            if (version_status == NV_ENC_SUCCESS) {
                api_version = driver_version < NVENCAPI_VERSION ? driver_version : NVENCAPI_VERSION;
                report_status(env, "NVENC driver API "
                                   + std::to_string(driver_version & 0xff) + "."
                                   + std::to_string((driver_version >> 24) & 0xff)
                                   + ", header API "
                                   + std::to_string(NVENCAPI_VERSION & 0xff) + "."
                                   + std::to_string((NVENCAPI_VERSION >> 24) & 0xff));
            }
        }

        std::memset(&api_, 0, sizeof(api_));
        api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        NVENCSTATUS status = create_instance(&api_);
        if (status != NV_ENC_SUCCESS) {
            report_status(env, "NvEncodeAPICreateInstance failed: " + nvenc_status(status));
            return false;
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
        session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        session.device = device;
        session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        session.apiVersion = api_version;
        status = api_.nvEncOpenEncodeSessionEx(&session, &encoder_);
        if (status != NV_ENC_SUCCESS) {
            report_status(env, "NVENC D3D11 session open failed: " + nvenc_status(status));
            return false;
        }

        GUID selected_preset = NV_ENC_PRESET_P2_GUID;
        NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
        if (!load_preset_config(env, selected_preset, tuning)) {
            report_status(env, "NVENC preset config unavailable, using minimal manual config");
            std::memset(&config_, 0, sizeof(config_));
            config_.version = NV_ENC_CONFIG_VER;
        }
        config_.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        uint32_t idr_period = fps_ >= 1 ? fps_ : 1;
        config_.gopLength = idr_period;
        config_.frameIntervalP = 1;
        config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        config_.rcParams.averageBitRate = bitrate_;
        config_.rcParams.maxBitRate = bitrate_;
        config_.rcParams.vbvBufferSize = bitrate_ * 2;
        config_.rcParams.vbvInitialDelay = bitrate_;
        config_.rcParams.enableAQ = 1;
        config_.rcParams.enableTemporalAQ = 1;
        config_.rcParams.enableLookahead = 0;
        config_.rcParams.lookaheadDepth = 0;
        config_.rcParams.zeroReorderDelay = 1;
        config_.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        config_.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;
        config_.encodeCodecConfig.h264Config.idrPeriod = idr_period;
        config_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        config_.encodeCodecConfig.h264Config.disableSPSPPS = 0;
        config_.encodeCodecConfig.h264Config.outputAUD = 1;

        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = NV_ENC_CODEC_H264_GUID;
        init.presetGUID = selected_preset;
        init.tuningInfo = tuning;
        init.encodeWidth = width_;
        init.encodeHeight = height_;
        init.darWidth = width_;
        init.darHeight = height_;
        init.frameRateNum = fps_;
        init.frameRateDen = 1;
        init.enableEncodeAsync = 1;
        init.enablePTD = 1;
        init.maxEncodeWidth = width_;
        init.maxEncodeHeight = height_;
        init.encodeConfig = &config_;

        status = api_.nvEncInitializeEncoder(encoder_, &init);
        if (status != NV_ENC_SUCCESS) {
            report_status(env, "NVENC initialize with custom config failed: " + nvenc_status(status));
            init.encodeConfig = nullptr;
            status = api_.nvEncInitializeEncoder(encoder_, &init);
            if (status != NV_ENC_SUCCESS) {
                report_status(env, "NVENC initialize failed: " + nvenc_status(status));
                return false;
            }
        }

        device->GetImmediateContext(context_.put());
        if (!context_) {
            report_status(env, "NVENC D3D11 immediate context unavailable");
            return false;
        }

        D3D11_TEXTURE2D_DESC input_desc{};
        input_desc.Width = width_;
        input_desc.Height = height_;
        input_desc.MipLevels = 1;
        input_desc.ArraySize = 1;
        input_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        input_desc.SampleDesc.Count = 1;
        input_desc.Usage = D3D11_USAGE_DEFAULT;
        input_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        for (size_t i = 0; i < 16; ++i) {
            NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
            bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            status = api_.nvEncCreateBitstreamBuffer(encoder_, &bitstream);
            if (status != NV_ENC_SUCCESS) {
                report_status(env, "NVENC bitstream buffer create failed: " + nvenc_status(status));
                return false;
            }

            ComPtr<ID3D11Texture2D> input_texture;
            HRESULT hr = device->CreateTexture2D(&input_desc, nullptr, input_texture.put());
            if (FAILED(hr)) {
                api_.nvEncDestroyBitstreamBuffer(encoder_, bitstream.bitstreamBuffer);
                report_status(env, "NVENC input texture create failed: " + hr_hex(hr));
                return false;
            }

            NV_ENC_REGISTER_RESOURCE registered{};
            registered.version = NV_ENC_REGISTER_RESOURCE_VER;
            registered.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            registered.resourceToRegister = input_texture.get();
            registered.width = width_;
            registered.height = height_;
            registered.pitch = 0;
            registered.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
            status = api_.nvEncRegisterResource(encoder_, &registered);
            if (status != NV_ENC_SUCCESS) {
                api_.nvEncDestroyBitstreamBuffer(encoder_, bitstream.bitstreamBuffer);
                report_status(env, "NVENC register input texture failed: " + nvenc_status(status));
                return false;
            }

            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) {
                api_.nvEncUnregisterResource(encoder_, registered.registeredResource);
                api_.nvEncDestroyBitstreamBuffer(encoder_, bitstream.bitstreamBuffer);
                report_status(env, "CreateEvent for NVENC failed");
                return false;
            }
            NV_ENC_EVENT_PARAMS event_params{};
            event_params.version = NV_ENC_EVENT_PARAMS_VER;
            event_params.completionEvent = event;
            status = api_.nvEncRegisterAsyncEvent(encoder_, &event_params);
            if (status != NV_ENC_SUCCESS) {
                CloseHandle(event);
                api_.nvEncUnregisterResource(encoder_, registered.registeredResource);
                api_.nvEncDestroyBitstreamBuffer(encoder_, bitstream.bitstreamBuffer);
                report_status(env, "NVENC async event register failed: " + nvenc_status(status));
                return false;
            }
            slots_.push_back(BitstreamSlot{
                    bitstream.bitstreamBuffer,
                    event,
                    std::move(input_texture),
                    registered.registeredResource
            });
        }
        report_status(env, "NVENC D3D11 texture encoder ready (" + std::to_string(width_) + "x"
                           + std::to_string(height_) + " @" + std::to_string(fps_) + "fps, "
                           + std::to_string(bitrate_ / 1'000'000) + "Mbps)");
        return true;
    }

    bool encode(JNIEnv *env, ID3D11Texture2D *texture, bool force_idr, std::mutex *source_mutex = nullptr) {
        if (!encoder_ || !texture) {
            return false;
        }

        if (!drain_ready(env)) {
            return false;
        }
        if (pending_.size() + 1 >= slots_.size() && !drain_oldest(env, true)) {
            return false;
        }
        size_t slot_index = next_slot_;
        BitstreamSlot &slot = slots_[slot_index];
        next_slot_ = (next_slot_ + 1) % slots_.size();
        auto copy_start = std::chrono::steady_clock::now();
        if (source_mutex) {
            std::lock_guard<std::mutex> guard(*source_mutex);
            context_->CopyResource(slot.input.get(), texture);
        } else {
            context_->CopyResource(slot.input.get(), texture);
        }
        record_timing(copy_us_total_, copy_us_max_, copy_start);

        NV_ENC_MAP_INPUT_RESOURCE mapped{};
        mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapped.registeredResource = slot.input_registered;
        auto map_start = std::chrono::steady_clock::now();
        NVENCSTATUS status = api_.nvEncMapInputResource(encoder_, &mapped);
        record_timing(map_us_total_, map_us_max_, map_start);
        if (status != NV_ENC_SUCCESS) {
            report_status(env, "NVENC map texture failed: " + nvenc_status(status));
            return false;
        }

        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.inputBuffer = mapped.mappedResource;
        picture.bufferFmt = mapped.mappedBufferFmt;
        picture.inputWidth = width_;
        picture.inputHeight = height_;
        picture.outputBitstream = slot.output;
        const uint64_t submitted_frame_index = frame_index_;
        picture.inputTimeStamp = submitted_frame_index;
        picture.completionEvent = slot.event;
        picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        if (force_idr) {
            picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }

        auto submit_start = std::chrono::steady_clock::now();
        status = api_.nvEncEncodePicture(encoder_, &picture);
        record_timing(submit_us_total_, submit_us_max_, submit_start);
        api_.nvEncUnmapInputResource(encoder_, mapped.mappedResource);
        if (status != NV_ENC_SUCCESS) {
            report_status(env, "NVENC encode picture failed: " + nvenc_status(status));
            return false;
        }

        ++frame_index_;
        ++timing_samples_;
        pending_.push_back(PendingBitstream{slot_index, force_idr, submitted_frame_index});
        return drain_ready(env);
    }

    bool drain(JNIEnv *env) {
        bool ok = drain_ready(env);
        while (ok && !pending_.empty()) {
            ok = drain_oldest(env, true);
        }
        return ok;
    }

    uint64_t published_frames() const {
        return published_frames_;
    }

    std::string take_timing_report() {
        if (timing_samples_ == 0) {
            return "";
        }
        auto avg = [this](uint64_t total) { return total / timing_samples_; };
        std::string report = "nvenc timing us avg/max copy="
                + std::to_string(avg(copy_us_total_)) + "/" + std::to_string(copy_us_max_)
                + ", map=" + std::to_string(avg(map_us_total_)) + "/" + std::to_string(map_us_max_)
                + ", submit=" + std::to_string(avg(submit_us_total_)) + "/" + std::to_string(submit_us_max_)
                + ", wait=" + std::to_string(avg(wait_us_total_)) + "/" + std::to_string(wait_us_max_)
                + ", publish=" + std::to_string(avg(publish_us_total_)) + "/" + std::to_string(publish_us_max_);
        timing_samples_ = 0;
        copy_us_total_ = map_us_total_ = submit_us_total_ = wait_us_total_ = publish_us_total_ = 0;
        copy_us_max_ = map_us_max_ = submit_us_max_ = wait_us_max_ = publish_us_max_ = 0;
        return report;
    }

    ~NvencTextureEncoder() {
        if (encoder_) {
            pending_.clear();
            for (BitstreamSlot &slot : slots_) {
                NV_ENC_EVENT_PARAMS event_params{};
                event_params.version = NV_ENC_EVENT_PARAMS_VER;
                event_params.completionEvent = slot.event;
                api_.nvEncUnregisterAsyncEvent(encoder_, &event_params);
                api_.nvEncUnregisterResource(encoder_, slot.input_registered);
                api_.nvEncDestroyBitstreamBuffer(encoder_, slot.output);
                CloseHandle(slot.event);
            }
            slots_.clear();
            api_.nvEncDestroyEncoder(encoder_);
            encoder_ = nullptr;
        }
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
    }

private:
    struct PendingBitstream {
        size_t slot_index;
        bool force_idr;
        uint64_t frame_index;
    };

    struct BitstreamSlot {
        NV_ENC_OUTPUT_PTR output;
        HANDLE event;
        ComPtr<ID3D11Texture2D> input;
        NV_ENC_REGISTERED_PTR input_registered;
    };

    bool load_preset_config(JNIEnv *env, GUID &selected_preset, NV_ENC_TUNING_INFO tuning) {
        const GUID candidates[] = {
                selected_preset,
                NV_ENC_PRESET_P2_GUID,
                NV_ENC_PRESET_P1_GUID
        };
        for (const GUID &preset_guid : candidates) {
            NV_ENC_PRESET_CONFIG preset{};
            preset.version = NV_ENC_PRESET_CONFIG_VER;
            preset.presetCfg.version = NV_ENC_CONFIG_VER;
            NVENCSTATUS status = api_.nvEncGetEncodePresetConfigEx(
                    encoder_, NV_ENC_CODEC_H264_GUID, preset_guid, tuning, &preset);
            if (status == NV_ENC_SUCCESS) {
                config_ = preset.presetCfg;
                config_.version = NV_ENC_CONFIG_VER;
                selected_preset = preset_guid;
                report_status(env, "NVENC preset config loaded");
                return true;
            }
            report_status(env, "NVENC preset config rejected: " + nvenc_status(status));
        }
        return false;
    }

    bool drain_ready(JNIEnv *env) {
        while (!pending_.empty()) {
            BitstreamSlot &slot = slots_[pending_.front().slot_index];
            DWORD wait_result = WaitForSingleObject(slot.event, 0);
            if (wait_result == WAIT_TIMEOUT) {
                return true;
            }
            if (wait_result != WAIT_OBJECT_0) {
                report_status(env, "NVENC async poll failed");
                return false;
            }
            if (!drain_front_after_signal(env)) {
                return false;
            }
        }
        return true;
    }

    bool drain_oldest(JNIEnv *env, bool wait) {
        if (pending_.empty()) {
            return true;
        }
        BitstreamSlot &slot = slots_[pending_.front().slot_index];
        auto wait_start = std::chrono::steady_clock::now();
        DWORD wait_result = WaitForSingleObject(slot.event, wait ? INFINITE : 0);
        record_timing(wait_us_total_, wait_us_max_, wait_start);
        if (wait_result == WAIT_TIMEOUT) {
            return true;
        }
        if (wait_result != WAIT_OBJECT_0) {
            report_status(env, "NVENC async wait failed");
            return false;
        }
        return drain_front_after_signal(env);
    }

    bool drain_front_after_signal(JNIEnv *env) {
        PendingBitstream pending = pending_.front();
        pending_.pop_front();
        BitstreamSlot &slot = slots_[pending.slot_index];
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = slot.output;
        lock.doNotWait = 0;
        NVENCSTATUS status = api_.nvEncLockBitstream(encoder_, &lock);
        if (status != NV_ENC_SUCCESS) {
            report_status(env, "NVENC lock bitstream failed: " + nvenc_status(status));
            return false;
        }
        bool keyframe = pending.force_idr || lock.pictureType == NV_ENC_PIC_TYPE_IDR
                || contains_h264_idr(static_cast<const uint8_t *>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes);
        std::vector<uint8_t> packet = build_packet(static_cast<const uint8_t *>(lock.bitstreamBufferPtr),
                                                   lock.bitstreamSizeInBytes, keyframe, pending.frame_index);
        api_.nvEncUnlockBitstream(encoder_, slot.output);
        auto publish_start = std::chrono::steady_clock::now();
        bool sent = publish_packet(env, packet);
        record_timing(publish_us_total_, publish_us_max_, publish_start);
        if (sent) {
            ++published_frames_;
        }
        return sent;
    }

    std::vector<uint8_t> build_packet(const uint8_t *payload, uint32_t payload_size, bool keyframe,
                                      uint64_t frame_index) {
        std::vector<uint8_t> packet(VIDEO_HEADER_BYTES + payload_size);
        put_u32(packet, 0, VIDEO_MAGIC);
        put_u32(packet, 4, VIDEO_VERSION);
        put_u32(packet, 8, VIDEO_CODEC_H264_ANNEX_B);
        put_u32(packet, 12, keyframe ? VIDEO_FLAG_KEYFRAME : 0);
        put_u32(packet, 16, width_);
        put_u32(packet, 20, height_);
        put_u64(packet, 24, static_cast<uint64_t>(frame_index * 1'000'000ULL / fps_));
        put_u32(packet, 32, payload_size);
        put_u32(packet, 36, 0);
        std::memcpy(packet.data() + VIDEO_HEADER_BYTES, payload, payload_size);
        return packet;
    }

    bool publish_packet(JNIEnv *env, const std::vector<uint8_t> &packet) {
        if (!env) {
            return false;
        }
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(packet.size()));
        if (!bytes) {
            return false;
        }
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(packet.size()), reinterpret_cast<const jbyte *>(packet.data()));
        env->CallVoidMethod(g_bridge, g_write, bytes, 0, static_cast<jint>(packet.size()));
        env->DeleteLocalRef(bytes);
        return !env->ExceptionCheck();
    }

    static void record_timing(uint64_t &total, uint64_t &max_value, std::chrono::steady_clock::time_point start) {
        uint64_t elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        total += elapsed;
        if (elapsed > max_value) {
            max_value = elapsed;
        }
    }

    uint32_t width_;
    uint32_t height_;
    uint32_t fps_;
    uint32_t bitrate_;
    uint64_t frame_index_ = 0;
    uint64_t published_frames_ = 0;
    HMODULE module_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void *encoder_ = nullptr;
    ComPtr<ID3D11DeviceContext> context_;
    std::vector<BitstreamSlot> slots_;
    std::deque<PendingBitstream> pending_;
    size_t next_slot_ = 0;
    NV_ENC_CONFIG config_{};
    uint64_t timing_samples_ = 0;
    uint64_t copy_us_total_ = 0;
    uint64_t map_us_total_ = 0;
    uint64_t submit_us_total_ = 0;
    uint64_t wait_us_total_ = 0;
    uint64_t publish_us_total_ = 0;
    uint64_t copy_us_max_ = 0;
    uint64_t map_us_max_ = 0;
    uint64_t submit_us_max_ = 0;
    uint64_t wait_us_max_ = 0;
    uint64_t publish_us_max_ = 0;
};

static inline uint8_t clamp_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

static void bgra_to_nv12(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride, uint8_t *nv12) {
    uint8_t *y_plane = nv12;
    uint8_t *uv_plane = nv12 + width * height;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = bgra + y * stride;
        uint8_t *y_row = y_plane + y * width;
        for (uint32_t x = 0; x < width; ++x) {
            const int b = row[x * 4 + 0];
            const int g = row[x * 4 + 1];
            const int r = row[x * 4 + 2];
            y_row[x] = clamp_byte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    for (uint32_t y = 0; y < height; y += 2) {
        uint8_t *uv_row = uv_plane + (y / 2) * width;
        for (uint32_t x = 0; x < width; x += 2) {
            int sum_u = 0;
            int sum_v = 0;
            int samples = 0;
            for (uint32_t oy = 0; oy < 2 && y + oy < height; ++oy) {
                const uint8_t *row = bgra + (y + oy) * stride;
                for (uint32_t ox = 0; ox < 2 && x + ox < width; ++ox) {
                    const uint8_t *pixel = row + (x + ox) * 4;
                    const int b = pixel[0];
                    const int g = pixel[1];
                    const int r = pixel[2];
                    sum_u += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    sum_v += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                    ++samples;
                }
            }
            uv_row[x] = clamp_byte(sum_u / samples);
            uv_row[x + 1] = clamp_byte(sum_v / samples);
        }
    }
}

static bool open_wgc_monitor(JNIEnv *env, int display_index, int adapter_index, WgcContext &result) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
        report_status(env, "WGC is not supported on this system");
        return false;
    }

    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr,
                        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                            auto *items = reinterpret_cast<std::vector<HMONITOR> *>(data);
                            items->push_back(monitor);
                            return TRUE;
                        },
                        reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        report_status(env, "EnumDisplayMonitors found no monitors");
        return false;
    }
    int selected_index = display_index;
    if (selected_index < 0) {
        selected_index = 0;
    }
    if (selected_index >= static_cast<int>(monitors.size())) {
        selected_index = static_cast<int>(monitors.size() - 1);
    }
    uint32_t selected = static_cast<uint32_t>(selected_index);
    HMONITOR monitor = monitors[selected];

    auto factory = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    HRESULT hr = factory->CreateForMonitor(
            monitor,
            winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            winrt::put_abi(item));
    if (FAILED(hr) || !item) {
        report_status(env, "WGC CreateForMonitor failed: " + hr_hex(hr));
        return false;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!create_default_device(adapter_index, device, context, hr)) {
        report_status(env, "WGC D3D11CreateDevice failed: " + hr_hex(hr));
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(dxgi_device.put()));
    if (FAILED(hr)) {
        report_status(env, "WGC IDXGIDevice query failed: " + hr_hex(hr));
        return false;
    }

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice direct3d_device{nullptr};
    hr = CreateDirect3D11DeviceFromDXGIDevice(
            dxgi_device.get(),
            reinterpret_cast<::IInspectable **>(winrt::put_abi(direct3d_device)));
    if (FAILED(hr) || !direct3d_device) {
        report_status(env, "CreateDirect3D11DeviceFromDXGIDevice failed: " + hr_hex(hr));
        return false;
    }

    auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) {
        report_status(env, "WGC item has invalid size");
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = static_cast<UINT>(size.Width);
    staging_desc.Height = static_cast<UINT>(size.Height);
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    hr = device->CreateTexture2D(&staging_desc, nullptr, staging.put());
    if (FAILED(hr)) {
        report_status(env, "WGC CreateTexture2D staging failed: " + hr_hex(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC latest_desc{};
    latest_desc.Width = static_cast<UINT>(size.Width);
    latest_desc.Height = static_cast<UINT>(size.Height);
    latest_desc.MipLevels = 1;
    latest_desc.ArraySize = 1;
    latest_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    latest_desc.SampleDesc.Count = 1;
    latest_desc.Usage = D3D11_USAGE_DEFAULT;
    latest_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> latest;
    hr = device->CreateTexture2D(&latest_desc, nullptr, latest.put());
    if (FAILED(hr)) {
        report_status(env, "WGC CreateTexture2D latest failed: " + hr_hex(hr));
        return false;
    }

    auto frame_pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            direct3d_device,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            size);
    auto session = frame_pool.CreateCaptureSession(item);
    session.IsCursorCaptureEnabled(true);

    result.device = std::move(device);
    result.context = std::move(context);
    result.staging = std::move(staging);
    result.latest = std::move(latest);
    result.frame_pool = frame_pool;
    result.session = session;
    result.source_width = static_cast<uint32_t>(size.Width);
    result.source_height = static_cast<uint32_t>(size.Height);
    result.frame_arrived = result.frame_pool.FrameArrived(
            winrt::auto_revoke,
            [&result](winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender,
                      winrt::Windows::Foundation::IInspectable const &) {
                for (;;) {
                    auto frame = sender.TryGetNextFrame();
                    if (!frame) {
                        break;
                    }
                    auto surface = frame.Surface();
                    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    ComPtr<ID3D11Texture2D> texture;
                    HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(texture.put()));
                    if (FAILED(hr)) {
                        continue;
                    }
                    std::lock_guard<std::mutex> guard(result.texture_mutex);
                    result.context->CopyResource(result.latest.get(), texture.get());
                    result.has_latest_frame = true;
                    ++result.captured_frames;
                }
            });
    session.StartCapture();
    report_status(env, "WGC capture started on display " + std::to_string(selected)
                       + " (" + std::to_string(result.source_width) + "x"
                       + std::to_string(result.source_height) + ")");
    return true;
}

static bool encode_bgra_frame(
        JNIEnv *env,
        H264Encoder &encoder,
        const uint8_t *bgra,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        std::vector<uint8_t> &nv12) {
    bgra_to_nv12(bgra, width, height, stride, nv12.data());
    return encoder.encode(env, nv12.data(), static_cast<uint32_t>(nv12.size()));
}

static FrameSendResult send_wgc_frame(JNIEnv *env, WgcContext &wgc, NvencTextureEncoder *nvenc, H264Encoder &encoder,
                                      uint32_t target_width, uint32_t target_height, uint32_t target_stride,
                                      std::vector<uint8_t> &scaled, std::vector<uint8_t> &nv12, uint64_t frame_index,
                                      uint32_t fps) {
    FrameSendResult result;
    uint64_t captured_frames;
    {
        std::lock_guard<std::mutex> guard(wgc.texture_mutex);
        if (!wgc.has_latest_frame) {
            return result;
        }
        captured_frames = wgc.captured_frames;
    }
    result.captured_new_frame = captured_frames != wgc.last_reported_frame;
    wgc.last_reported_frame = captured_frames;

    if (nvenc && target_width == wgc.source_width && target_height == wgc.source_height) {
        uint64_t idr_period = static_cast<uint64_t>(fps > 0 ? fps : 1);
        result.ok = nvenc->encode(env, wgc.latest.get(),
                                  frame_index % idr_period == 0,
                                  &wgc.texture_mutex);
        return result;
    }

    {
        std::lock_guard<std::mutex> guard(wgc.texture_mutex);
        wgc.context->CopyResource(wgc.staging.get(), wgc.latest.get());
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = wgc.context->Map(wgc.staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        report_status(env, "WGC Map staging texture failed: " + hr_hex(hr));
        return result;
    }

    bool sent;
    if (target_width == wgc.source_width && target_height == wgc.source_height) {
        sent = encode_bgra_frame(env, encoder, static_cast<const uint8_t *>(mapped.pData),
                                 wgc.source_width, wgc.source_height, mapped.RowPitch, nv12);
    } else {
        scale_bgra(
                static_cast<const uint8_t *>(mapped.pData),
                wgc.source_width,
                wgc.source_height,
                mapped.RowPitch,
                scaled.data(),
                target_width,
                target_height,
                target_stride);
        sent = encode_bgra_frame(env, encoder, scaled.data(), target_width, target_height, target_stride, nv12);
    }
    wgc.context->Unmap(wgc.staging.get(), 0);
    result.ok = sent;
    result.published_packet = sent;
    return result;
}

class AudioLoopbackCapture {
public:
    void start() {
        worker_ = std::thread([this]() { run(); });
    }

    void stop() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    ~AudioLoopbackCapture() {
        stop();
    }

private:
    void run() {
        JNIEnv *env = nullptr;
        if (g_vm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) != JNI_OK) {
            return;
        }
        HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool com_initialized = SUCCEEDED(com_hr);

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(enumerator.put()));
        if (FAILED(hr)) {
            report_status(env, "WASAPI device enumerator failed: " + hr_hex(hr));
            finish(com_initialized);
            return;
        }

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
        if (FAILED(hr)) {
            report_status(env, "WASAPI default render endpoint unavailable: " + hr_hex(hr));
            finish(com_initialized);
            return;
        }

        ComPtr<IAudioClient> audio_client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(audio_client.put()));
        if (FAILED(hr)) {
            report_status(env, "WASAPI audio client activate failed: " + hr_hex(hr));
            finish(com_initialized);
            return;
        }

        WAVEFORMATEX *mix = nullptr;
        hr = audio_client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) {
            report_status(env, "WASAPI mix format unavailable: " + hr_hex(hr));
            finish(com_initialized);
            return;
        }

        HANDLE samples_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!samples_ready) {
            CoTaskMemFree(mix);
            report_status(env, "WASAPI event create failed");
            finish(com_initialized);
            return;
        }

        REFERENCE_TIME buffer_duration = 10'000'000;
        hr = audio_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                buffer_duration,
                0,
                mix,
                nullptr);
        if (FAILED(hr)) {
            CloseHandle(samples_ready);
            CoTaskMemFree(mix);
            report_status(env, "WASAPI loopback initialize failed: " + hr_hex(hr));
            finish(com_initialized);
            return;
        }

        hr = audio_client->SetEventHandle(samples_ready);
        if (FAILED(hr)) {
            CloseHandle(samples_ready);
            CoTaskMemFree(mix);
            report_status(env, "WASAPI event handle failed: " + hr_hex(hr));
            finish(com_initialized);
            return;
        }

        ComPtr<IAudioCaptureClient> capture_client;
        hr = audio_client->GetService(IID_PPV_ARGS(capture_client.put()));
        if (FAILED(hr)) {
            CloseHandle(samples_ready);
            CoTaskMemFree(mix);
            report_status(env, "WASAPI capture client unavailable: " + hr_hex(hr));
            finish(com_initialized);
            return;
        }

        const uint32_t sample_rate = mix->nSamplesPerSec;
        const uint32_t channels = mix->nChannels;
        const WORD bits_per_sample = mix->wBitsPerSample;
        const WORD bytes_per_sample = bits_per_sample / 8;
        const WORD block_align = mix->nBlockAlign;
        GUID subtype = {};
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            subtype = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix)->SubFormat;
        } else if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            subtype = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        } else {
            subtype = KSDATAFORMAT_SUBTYPE_PCM;
        }

        if (channels == 0 || sample_rate == 0 || bytes_per_sample == 0 || block_align == 0) {
            CloseHandle(samples_ready);
            CoTaskMemFree(mix);
            report_status(env, "WASAPI invalid mix format");
            finish(com_initialized);
            return;
        }

        hr = audio_client->Start();
        if (FAILED(hr)) {
            CloseHandle(samples_ready);
            CoTaskMemFree(mix);
            report_status(env, "WASAPI loopback start failed: " + hr_hex(hr));
            finish(com_initialized);
            return;
        }
        report_status(env, "WASAPI loopback audio started (" + std::to_string(sample_rate) + "Hz, "
                           + std::to_string(channels) + "ch)");

        uint64_t timestamp_micros = 0;
        std::vector<float> float_samples;
        while (g_running.load()) {
            DWORD wait = WaitForSingleObject(samples_ready, 200);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
                break;
            }
            UINT32 packet_frames = 0;
            hr = capture_client->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                report_status(env, "WASAPI packet size failed: " + hr_hex(hr));
                break;
            }
            while (packet_frames > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    report_status(env, "WASAPI get buffer failed: " + hr_hex(hr));
                    break;
                }
                if (frames > 0) {
                    float_samples.assign(static_cast<size_t>(frames) * channels, 0.0f);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data) {
                        if (!convert_to_float(data, frames, channels, block_align, bytes_per_sample, bits_per_sample,
                                              subtype, float_samples)) {
                            capture_client->ReleaseBuffer(frames);
                            report_status(env, "WASAPI unsupported mix format");
                            audio_client->Stop();
                            CloseHandle(samples_ready);
                            CoTaskMemFree(mix);
                            finish(com_initialized);
                            return;
                        }
                    }
                    publish_audio(env, float_samples, sample_rate, channels, timestamp_micros);
                    timestamp_micros += static_cast<uint64_t>(frames) * 1'000'000ULL / sample_rate;
                }
                capture_client->ReleaseBuffer(frames);
                hr = capture_client->GetNextPacketSize(&packet_frames);
                if (FAILED(hr)) {
                    packet_frames = 0;
                }
            }
        }

        audio_client->Stop();
        CloseHandle(samples_ready);
        CoTaskMemFree(mix);
        finish(com_initialized);
    }

    static bool convert_to_float(const BYTE *data, UINT32 frames, uint32_t channels, WORD block_align,
                                 WORD bytes_per_sample, WORD bits_per_sample, const GUID &subtype,
                                 std::vector<float> &out) {
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const BYTE *frame_data = data + static_cast<size_t>(frame) * block_align;
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const BYTE *sample = frame_data + static_cast<size_t>(channel) * bytes_per_sample;
                float value = 0.0f;
                if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && bits_per_sample == 32) {
                    std::memcpy(&value, sample, sizeof(float));
                } else if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_PCM) && bits_per_sample == 16) {
                    int16_t raw = 0;
                    std::memcpy(&raw, sample, sizeof(raw));
                    value = static_cast<float>(raw) / 32768.0f;
                } else if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_PCM) && bits_per_sample == 24) {
                    int32_t raw = (static_cast<int32_t>(sample[0]))
                            | (static_cast<int32_t>(sample[1]) << 8)
                            | (static_cast<int32_t>(sample[2]) << 16);
                    if (raw & 0x00800000) {
                        raw |= static_cast<int32_t>(0xff000000);
                    }
                    value = static_cast<float>(raw) / 8388608.0f;
                } else if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_PCM) && bits_per_sample == 32) {
                    int32_t raw = 0;
                    std::memcpy(&raw, sample, sizeof(raw));
                    value = static_cast<float>(raw) / 2147483648.0f;
                } else {
                    return false;
                }
                out[static_cast<size_t>(frame) * channels + channel] = value;
            }
        }
        return true;
    }

    static void publish_audio(JNIEnv *env, const std::vector<float> &samples, uint32_t sample_rate,
                              uint32_t channels, uint64_t timestamp_micros) {
        uint32_t payload_size = static_cast<uint32_t>(samples.size() * sizeof(float));
        std::vector<uint8_t> packet(AUDIO_HEADER_BYTES + payload_size);
        put_u32(packet, 0, AUDIO_MAGIC);
        put_u32(packet, 4, AUDIO_VERSION);
        put_u32(packet, 8, AUDIO_CODEC_PCM_FLOAT32);
        put_u32(packet, 12, 0);
        put_u32(packet, 16, sample_rate);
        put_u32(packet, 20, channels);
        put_u64(packet, 24, timestamp_micros);
        put_u32(packet, 32, payload_size);
        put_u32(packet, 36, 0);
        std::memcpy(packet.data() + AUDIO_HEADER_BYTES, samples.data(), payload_size);
        publish_native_packet(env, packet);
    }

    static void finish(bool com_initialized) {
        if (com_initialized) {
            CoUninitialize();
        }
        g_vm->DetachCurrentThread();
    }

    std::thread worker_;
};

static void capture_loop(CaptureConfig config) {
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) != JNI_OK) {
        return;
    }
    HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(com_hr);
    report_status(env, "native capture thread started");
    AudioLoopbackCapture audio_capture;
    audio_capture.start();

    DuplicationContext dxgi;
    WgcContext wgc;
    bool use_wgc = false;
    uint32_t source_width = 0;
    uint32_t source_height = 0;

    if (open_wgc_monitor(env, config.display_index, config.adapter_index, wgc)) {
        use_wgc = true;
        source_width = wgc.source_width;
        source_height = wgc.source_height;
    } else {
        report_status(env, "WGC unavailable, trying DXGI duplication");
        if (!open_first_working_duplication(env, config.display_index, config.adapter_index, dxgi)) {
            report_status(env, "no working native capture backend found");
            if (com_initialized) {
                CoUninitialize();
            }
            g_vm->DetachCurrentThread();
            return;
        }
        source_width = dxgi.source_width;
        source_height = dxgi.source_height;
    }

    const uint32_t requested_width = config.width > 0 ? static_cast<uint32_t>(config.width) : source_width;
    const uint32_t requested_height = config.height > 0 ? static_cast<uint32_t>(config.height) : source_height;
    const uint32_t target_width = requested_width & ~1u;
    const uint32_t target_height = requested_height & ~1u;
    const uint32_t target_stride = target_width * 4;
    report_status(env, "capture output " + std::to_string(source_width) + "x" + std::to_string(source_height)
                       + " -> " + std::to_string(target_width) + "x" + std::to_string(target_height));

    std::vector<uint8_t> scaled(target_stride * target_height);
    std::vector<uint8_t> nv12(target_width * target_height * 3 / 2);
    std::unique_ptr<NvencTextureEncoder> nvenc;
    ID3D11Device *capture_device = use_wgc ? wgc.device.get() : dxgi.device.get();
    if (target_width == source_width && target_height == source_height) {
        auto candidate = std::make_unique<NvencTextureEncoder>(
                target_width, target_height, static_cast<uint32_t>(config.fps), static_cast<uint32_t>(config.bitrate));
        if (candidate->open(env, capture_device)) {
            nvenc = std::move(candidate);
        } else {
            report_status(env, "NVENC texture path unavailable, using CPU-fed H.264 encoder");
        }
    } else {
        report_status(env, "NVENC texture path requires same-size output; using CPU-fed H.264 encoder until GPU scaler is enabled");
    }
    auto encoder = std::make_unique<H264Encoder>(
            target_width, target_height, static_cast<uint32_t>(config.fps), static_cast<uint32_t>(config.bitrate));
    if (!nvenc && !encoder->open(env)) {
        encoder.reset();
        if (com_initialized) {
            CoUninitialize();
        }
        g_vm->DetachCurrentThread();
        return;
    }
    const auto frame_interval = std::chrono::microseconds(1'000'000 / (config.fps > 0 ? config.fps : 30));
    uint64_t frames_sent = 0;
    uint64_t frames_at_last_report = 0;
    uint64_t new_frames_seen = 0;
    uint64_t new_frames_at_last_report = 0;
    uint64_t packets_at_last_report = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (g_running.load()) {
        auto frame_start = std::chrono::steady_clock::now();
        if (use_wgc) {
            FrameSendResult send_result = send_wgc_frame(env, wgc, nvenc.get(), *encoder, target_width, target_height,
                                                         target_stride, scaled, nv12, frames_sent,
                                                         static_cast<uint32_t>(config.fps));
            if (send_result.captured_new_frame) {
                ++new_frames_seen;
            }
            if (send_result.ok) {
                ++frames_sent;
                if (frames_sent == 1 || frames_sent % 120 == 0) {
                    report_status(env, "frames submitted: " + std::to_string(frames_sent));
                }
            }
            auto now = std::chrono::steady_clock::now();
            auto report_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();
            if (report_elapsed >= 1000) {
                uint64_t frames_delta = frames_sent - frames_at_last_report;
                uint64_t new_frames_delta = new_frames_seen - new_frames_at_last_report;
                uint64_t packets_now = nvenc ? nvenc->published_frames() : frames_sent;
                uint64_t packets_delta = packets_now - packets_at_last_report;
                uint64_t fps_now = frames_delta * 1000 / static_cast<uint64_t>(report_elapsed);
                uint64_t capture_fps_now = new_frames_delta * 1000 / static_cast<uint64_t>(report_elapsed);
                uint64_t publish_fps_now = packets_delta * 1000 / static_cast<uint64_t>(report_elapsed);
                report_status(env, "capture new fps: " + std::to_string(capture_fps_now)
                                   + ", encode submit fps: " + std::to_string(fps_now)
                                   + ", packet publish fps: " + std::to_string(publish_fps_now)
                                   + " (" + std::to_string(target_width) + "x" + std::to_string(target_height) + ")");
                if (nvenc && fps_now * 10 < static_cast<uint64_t>(config.fps > 0 ? config.fps : 60) * 9) {
                    std::string timing = nvenc->take_timing_report();
                    if (!timing.empty()) {
                        report_status(env, timing);
                    }
                } else if (nvenc) {
                    nvenc->take_timing_report();
                }
                frames_at_last_report = frames_sent;
                new_frames_at_last_report = new_frames_seen;
                packets_at_last_report = packets_now;
                last_report = now;
            }
            sleep_until_precise(frame_start + frame_interval);
            continue;
        }

        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        ComPtr<IDXGIResource> frame_resource;
        HRESULT hr = dxgi.duplication->AcquireNextFrame(100, &frame_info, frame_resource.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (FAILED(hr)) {
            report_status(env, "AcquireNextFrame failed: " + hr_hex(hr));
            break;
        }

        ComPtr<ID3D11Texture2D> frame;
        hr = frame_resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(frame.put()));
        if (SUCCEEDED(hr)) {
            if (nvenc && target_width == dxgi.source_width && target_height == dxgi.source_height) {
                bool sent = nvenc->encode(env, frame.get(),
                                          frames_sent % static_cast<uint64_t>(config.fps > 0 ? config.fps : 60) == 0);
                if (sent) {
                    ++frames_sent;
                    if (frames_sent == 1 || frames_sent % 120 == 0) {
                        report_status(env, "frames encoded: " + std::to_string(frames_sent));
                    }
                } else {
                    report_status(env, "NVENC encode frame failed");
                }
                dxgi.duplication->ReleaseFrame();
                sleep_until_precise(frame_start + frame_interval);
                continue;
            }
            dxgi.context->CopyResource(dxgi.staging.get(), frame.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = dxgi.context->Map(dxgi.staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                bool sent = false;
                if (target_width == dxgi.source_width && target_height == dxgi.source_height) {
                    sent = encode_bgra_frame(env, *encoder, static_cast<const uint8_t *>(mapped.pData),
                                             dxgi.source_width, dxgi.source_height, mapped.RowPitch, nv12);
                } else {
                    scale_bgra(
                            static_cast<const uint8_t *>(mapped.pData),
                            dxgi.source_width,
                            dxgi.source_height,
                            mapped.RowPitch,
                            scaled.data(),
                            target_width,
                            target_height,
                            target_stride);
                    sent = encode_bgra_frame(env, *encoder, scaled.data(), target_width, target_height, target_stride, nv12);
                }
                if (sent) {
                    ++frames_sent;
                    if (frames_sent == 1 || frames_sent % 120 == 0) {
                        report_status(env, "frames encoded: " + std::to_string(frames_sent));
                    }
                } else {
                    report_status(env, "encode frame failed");
                }
                dxgi.context->Unmap(dxgi.staging.get(), 0);
            } else {
                report_status(env, "Map staging texture failed: " + hr_hex(hr));
            }
        } else {
            report_status(env, "frame QueryInterface(ID3D11Texture2D) failed: " + hr_hex(hr));
        }

        dxgi.duplication->ReleaseFrame();
        auto now = std::chrono::steady_clock::now();
        auto report_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();
        if (report_elapsed >= 1000) {
            uint64_t frames_delta = frames_sent - frames_at_last_report;
            uint64_t fps_now = frames_delta * 1000 / static_cast<uint64_t>(report_elapsed);
            report_status(env, "encode fps: " + std::to_string(fps_now)
                               + " (" + std::to_string(target_width) + "x" + std::to_string(target_height) + ")");
            frames_at_last_report = frames_sent;
            last_report = now;
        }
        sleep_until_precise(frame_start + frame_interval);
    }

    audio_capture.stop();
    encoder.reset();
    if (com_initialized) {
        CoUninitialize();
    }
    g_vm->DetachCurrentThread();
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_top_ceroxe_webviewer_capture_NativeScreenCaptureBackend_nativeStart(
        JNIEnv *env,
        jclass,
        jstring,
        jint width,
        jint height,
        jint fps,
        jint bitrate,
        jint display_index,
        jint adapter_index,
        jobject bridge) {
    if (g_running.exchange(true)) {
        return;
    }

    env->GetJavaVM(&g_vm);
    g_bridge = env->NewGlobalRef(bridge);
    jclass bridge_class = env->GetObjectClass(bridge);
    g_write = env->GetMethodID(bridge_class, "write", "([BII)V");
    g_flush = env->GetMethodID(bridge_class, "flush", "()V");
    g_status = env->GetMethodID(bridge_class, "status", "(Ljava/lang/String;)V");

    CaptureConfig config{width, height, fps, bitrate, display_index, adapter_index};
    g_worker = std::thread([config]() { capture_loop(config); });
}

extern "C" JNIEXPORT void JNICALL
Java_top_ceroxe_webviewer_capture_NativeScreenCaptureBackend_nativeStop(JNIEnv *env, jclass) {
    g_running.store(false);
    if (g_worker.joinable()) {
        g_worker.join();
    }
    if (g_bridge) {
        env->DeleteGlobalRef(g_bridge);
        g_bridge = nullptr;
    }
    g_write = nullptr;
    g_flush = nullptr;
    g_status = nullptr;
    g_vm = nullptr;
}
