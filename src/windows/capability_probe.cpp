#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
struct Failure { const wchar_t* stage; HRESULT hr; };
void require(HRESULT hr, const wchar_t* stage) {
    if (FAILED(hr)) throw Failure{stage, hr};
}
void print_failure(const Failure& failure) {
    std::wcout << L"    NOT VERIFIED: " << failure.stage << L" (HRESULT 0x"
               << std::hex << std::setw(8) << std::setfill(L'0')
               << static_cast<unsigned long>(failure.hr) << std::dec
               << std::setfill(L' ') << L")\n";
}
struct Runtime {
    bool com = false, mf = false;
    void start() {
        require(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"COM initialization");
        com = true;
        require(MFStartup(MF_VERSION), L"Media Foundation startup");
        mf = true;
    }
    ~Runtime() { if (mf) MFShutdown(); if (com) CoUninitialize(); }
};
struct Activations {
    IMFActivate** items = nullptr;
    UINT32 count = 0;
    ~Activations() {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
};
struct ShutdownActivation {
    IMFActivate* activation;
    ~ShutdownActivation() { activation->ShutdownObject(); }
};
struct EndStream {
    IMFTransform* transform;
    ~EndStream() {
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
};
struct Preset { UINT32 width, height, fps, bitrate; };
constexpr Preset presets[] = {
    {1280, 720, 30, 6'000'000},
    {1920, 1080, 60, 12'000'000},
    {2560, 1440, 60, 20'000'000}
};
std::wstring encoder_name(IMFActivate* activation) {
    WCHAR* name = nullptr;
    UINT32 length = 0;
    if (FAILED(activation->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length)))
        return L"Unnamed hardware H.264 encoder";
    const std::wstring result(name, length);
    CoTaskMemFree(name);
    return result;
}
ComPtr<IMFMediaType> media_type(const Preset& p, bool output) {
    ComPtr<IMFMediaType> type;
    require(MFCreateMediaType(&type), L"create media type");
    require(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"video major type");
    require(type->SetGUID(MF_MT_SUBTYPE, output ? MFVideoFormat_H264 : MFVideoFormat_NV12), L"video subtype");
    require(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, p.width, p.height), L"frame size");
    require(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, p.fps, 1), L"frame rate");
    require(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), L"pixel aspect ratio");
    require(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), L"progressive video");
    if (output)
        require(type->SetUINT32(MF_MT_AVG_BITRATE, p.bitrate), L"output bitrate");
    return type;
}
void wait_for_input(IMFTransform* transform, DWORD input_id) {
    ComPtr<IMFMediaEventGenerator> events;
    require(transform->QueryInterface(IID_PPV_ARGS(&events)), L"async encoder event interface");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        require(hr, L"read encoder readiness event");
        HRESULT status = S_OK;
        require(event->GetStatus(&status), L"read encoder event status");
        require(status, L"encoder asynchronous error");
        MediaEventType type = MEUnknown;
        require(event->GetType(&type), L"read encoder event type");
        if (type == METransformNeedInput) {
            UINT32 requested = 0;
            require(event->GetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, &requested), L"input event stream ID");
            if (requested == input_id) return;
        }
    }
    throw Failure{L"encoder input readiness timed out after 3 seconds", HRESULT_FROM_WIN32(WAIT_TIMEOUT)};
}
void try_preset(IMFActivate* activation, ID3D11Device* device, const Preset& p) {
    ComPtr<IMFTransform> transform;
    require(activation->ActivateObject(IID_PPV_ARGS(&transform)), L"activate hardware encoder");
    ShutdownActivation shutdown{activation};
    ComPtr<IMFAttributes> attributes;
    require(transform->GetAttributes(&attributes), L"encoder attributes");
    require(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), L"unlock asynchronous encoder");
    UINT32 aware = FALSE;
    require(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware), L"query D3D11 awareness");
    if (!aware) throw Failure{L"encoder does not advertise D3D11 input", E_NOINTERFACE};
    std::wcout << L"    D3D11 awareness: yes\n";

    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> manager;
    require(MFCreateDXGIDeviceManager(&token, &manager), L"create DXGI device manager");
    require(manager->ResetDevice(device, token), L"attach adapter device to manager");
    require(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(manager.Get())), L"encoder accepts adapter's D3D manager");
    std::wcout << L"    Adapter device manager: accepted\n";

    DWORD inputs = 0, outputs = 0;
    require(transform->GetStreamCount(&inputs, &outputs), L"query stream count");
    if (inputs != 1 || outputs != 1)
        throw Failure{L"probe supports only single-input/single-output encoders", E_NOTIMPL};
    DWORD input_id = 0, output_id = 0;
    const HRESULT ids = transform->GetStreamIDs(1, &input_id, 1, &output_id);
    if (ids != E_NOTIMPL) require(ids, L"query stream IDs");
    auto output_type = media_type(p, true);
    require(transform->SetOutputType(output_id, output_type.Get(), 0), L"configure H.264 output preset");
    auto input_type = media_type(p, false);
    require(transform->SetInputType(input_id, input_type.Get(), 0), L"configure NV12 input preset");
    std::wcout << L"    NV12 -> H.264 type negotiation: accepted\n";

    // One initialized black NV12 frame. CPU initialization is diagnostic setup,
    // not the future capture path. The submitted sample wraps a GPU texture.
    const auto size = static_cast<std::size_t>(p.width) * p.height;
    std::vector<unsigned char> pixels(size + size / 2, 128);
    std::fill_n(pixels.begin(), size, static_cast<unsigned char>(16));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = p.width; desc.Height = p.height;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data(); data.SysMemPitch = p.width;
    ComPtr<ID3D11Texture2D> texture;
    require(device->CreateTexture2D(&desc, &data, &texture), L"create synthetic NV12 GPU texture");
    ComPtr<IMFMediaBuffer> buffer;
    require(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.Get(), 0, FALSE, &buffer),
            L"wrap GPU texture as Media Foundation buffer");
    ComPtr<IMF2DBuffer> two_d_buffer;
    require(buffer.As(&two_d_buffer), L"query 2D surface buffer");
    DWORD length = 0;
    require(two_d_buffer->GetContiguousLength(&length), L"query surface payload length");
    require(buffer->SetCurrentLength(length), L"set surface payload length");
    ComPtr<IMFSample> sample;
    require(MFCreateSample(&sample), L"create input sample");
    require(sample->AddBuffer(buffer.Get()), L"attach GPU surface buffer");
    require(sample->SetSampleTime(0), L"sample timestamp");
    require(sample->SetSampleDuration(10'000'000 / p.fps), L"sample duration");
    require(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), L"begin encoder streaming");
    EndStream end{transform.Get()};
    require(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), L"start encoder stream");
    wait_for_input(transform.Get(), input_id);
    require(transform->ProcessInput(input_id, sample.Get(), 0), L"submit GPU-backed NV12 sample");
    std::wcout << L"    GPU-backed sample: ACCEPTED (output encoding not verified)\n";
}
int run() {
    Runtime runtime;
    runtime.start();
    ComPtr<IDXGIFactory1> factory;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), L"DXGI factory");
    Activations encoders;
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    require(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE,
                     nullptr, &output, &encoders.items, &encoders.count), L"enumerate hardware H.264 encoders");
    std::wcout << L"Hardware H.264 registrations: " << encoders.count << L"\n";
    unsigned int accepted = 0, attempted = 0, adapters = 0;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT next = factory->EnumAdapters1(index, &adapter);
        if (next == DXGI_ERROR_NOT_FOUND) break;
        require(next, L"enumerate DXGI adapter");
        DXGI_ADAPTER_DESC1 info{};
        require(adapter->GetDesc1(&info), L"adapter description");
        std::wcout << L"\nAdapter " << index << L": " << info.Description
                   << L"\n  Vendor/device: 0x" << std::hex << info.VendorId << L"/0x" << info.DeviceId
                   << std::dec << L"\n  Dedicated video memory: " << info.DedicatedVideoMemory / (1024 * 1024)
                   << L" MiB (not current usage)\n";
        if (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            std::wcout << L"  Software adapter: skipped\n";
            continue;
        }
        ++adapters;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const HRESULT created = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(created)) { print_failure({L"create D3D11 video device", created}); continue; }
        ComPtr<ID3D10Multithread> protection;
        const HRESULT protected_device = device.As(&protection);
        if (FAILED(protected_device)) {
            print_failure({L"D3D multithread protection interface", protected_device});
            continue;
        }
        protection->SetMultithreadProtected(TRUE);
        for (UINT32 i = 0; i < encoders.count; ++i) {
            std::wcout << L"  Encoder candidate: " << encoder_name(encoders.items[i]) << L"\n";
            for (const auto& preset : presets) {
                ++attempted;
                std::wcout << L"  Preset " << preset.width << L'x' << preset.height << L'@'
                           << preset.fps << L", " << preset.bitrate / 1'000'000 << L" Mbps\n";
                try { try_preset(encoders.items[i], device.Get(), preset); ++accepted; }
                catch (const Failure& failure) { print_failure(failure); }
            }
        }
    }
    std::wcout << L"\nSummary: " << adapters << L" hardware adapters, " << accepted << L'/'
               << attempted << L" adapter/encoder/preset combinations accepted a GPU sample.\n"
               << L"Candidates are global registrations, not proof of encoder-to-adapter ownership.\n"
               << L"Acceptance does not prove encoded output, zero-copy internals, quality, or real-time FPS.\n";
    if (!accepted) {
        std::wcout << L"No verified candidate for this probe's NV12/D3D11 path. Check GPU drivers and run\n"
                   << L"locally on the Windows PC. This does not rule out other encoder APIs/formats.\n";
        return 2;
    }
    return 0;
}
} // namespace
int main() {
    std::wcout << L"Clipping Tool - Windows hardware capability probe\n"
               << L"Synthetic input only; no screen/audio capture and no files written.\n";
    try { return run(); }
    catch (const Failure& failure) {
        print_failure(failure);
        std::wcout << L"Probe could not complete. Media Foundation/Windows SDK components and GPU drivers\n"
                   << L"must be available; Windows N may require the Media Feature Pack.\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Probe failed: " << error.what() << '\n'; return 1;
    }
}
