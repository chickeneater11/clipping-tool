#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace clipping::windows {
constexpr UINT width = 1920, height = 1080, fps = 60;
struct Failure { const char* stage; HRESULT hr; };
inline void check(HRESULT hr, const char* stage) { if (FAILED(hr)) throw Failure{stage, hr}; }
struct Runtime {
    bool com = false, mf = false;
    void start() {
        check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "initialize COM"); com = true;
        check(MFStartup(MF_VERSION), "start Media Foundation"); mf = true;
    }
    ~Runtime() { if (mf) MFShutdown(); if (com) CoUninitialize(); }
};
// Declare before the writer so the writer releases first on every exit path.
struct OwnedMediaSink {
    ComPtr<IMFMediaSink> sink;
    ~OwnedMediaSink() { if (sink) sink->Shutdown(); }
};
struct FrameRelease {
    IDXGIOutputDuplication* duplication;
    ~FrameRelease() { duplication->ReleaseFrame(); }
};
inline ComPtr<IMFMediaType> video_type(bool encoded) {
    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(&type), "create media type");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "set video type");
    check(type->SetGUID(MF_MT_SUBTYPE, encoded ? MFVideoFormat_H264 : MFVideoFormat_NV12), "set subtype");
    check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height), "set frame size");
    check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1), "set frame rate");
    check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "set pixel aspect ratio");
    check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "set progressive scan");
    check(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "set BT709 matrix");
    check(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "set limited range");
    if (encoded) check(type->SetUINT32(MF_MT_AVG_BITRATE, 12'000'000), "set bitrate");
    return type;
}

struct Capture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> desktop;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    UINT source_width = 0, source_height = 0;
    bool have_frame = false;
    unsigned long long coalesced = 0;

    void start(UINT adapter_index, UINT output_index) {
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        check(factory->EnumAdapters1(adapter_index, &adapter), "select adapter index");
        ComPtr<IDXGIOutput> output;
        check(adapter->EnumOutputs(output_index, &output), "select output index");
        DXGI_OUTPUT_DESC display{};
        check(output->GetDesc(&display), "read display description");
        if (!display.AttachedToDesktop || display.Rotation != DXGI_MODE_ROTATION_IDENTITY)
            throw Failure{"first recorder requires an attached, unrotated display", E_NOTIMPL};
        std::wcout << L"Display: " << display.DeviceName << L"\n";
        check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
              D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
              nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "create D3D11 device");
        ComPtr<ID3D10Multithread> threading;
        check(device.As(&threading), "get D3D threading interface");
        threading->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIOutput1> output1;
        check(output.As(&output1), "get desktop duplication interface");
        check(output1->DuplicateOutput(device.Get(), &duplication), "duplicate desktop (local unlocked session required)");
        DXGI_OUTDUPL_DESC desc{};
        duplication->GetDesc(&desc);
        source_width = desc.ModeDesc.Width; source_height = desc.ModeDesc.Height;
        if (!source_width || !source_height || desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            throw Failure{"unsupported desktop surface format", E_NOTIMPL};
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = source_width; texture.Height = source_height;
        texture.MipLevels = 1; texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&texture, nullptr, &desktop), "allocate cached desktop texture");
        check(device.As(&video_device), "get video device");
        check(context.As(&video_context), "get video context");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = source_width; content.InputHeight = source_height;
        content.OutputWidth = width; content.OutputHeight = height;
        content.InputFrameRate = {fps, 1}; content.OutputFrameRate = {fps, 1};
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        check(video_device->CreateVideoProcessorEnumerator(&content, &enumerator), "create video processor enumerator");
        UINT flags = 0;
        check(enumerator->CheckVideoProcessorFormat(texture.Format, &flags), "check BGRA processing support");
        if (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) throw Failure{"BGRA video input unavailable", E_NOTIMPL};
        check(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags), "check NV12 processing support");
        if (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) throw Failure{"NV12 video output unavailable", E_NOTIMPL};
        check(video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor), "create GPU video processor");
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        check(video_device->CreateVideoProcessorInputView(desktop.Get(), enumerator.Get(), &view, &input_view), "create BGRA input view");
        video_context->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        const RECT source{0, 0, static_cast<LONG>(source_width), static_cast<LONG>(source_height)};
        const double scale = std::min(static_cast<double>(width) / source_width, static_cast<double>(height) / source_height);
        const LONG scaled_width = static_cast<LONG>(source_width * scale) & ~1L;
        const LONG scaled_height = static_cast<LONG>(source_height * scale) & ~1L;
        const LONG left = ((static_cast<LONG>(width) - scaled_width) / 2) & ~1L;
        const LONG top = ((static_cast<LONG>(height) - scaled_height) / 2) & ~1L;
        const RECT destination{left, top, left + scaled_width, top + scaled_height};
        const RECT target{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        video_context->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &source);
        video_context->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &destination);
        video_context->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &target);
        D3D11_VIDEO_COLOR black{}; black.RGBA.A = 1.0f;
        video_context->VideoProcessorSetOutputBackgroundColor(processor.Get(), FALSE, &black);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
        rgb.RGB_Range = 0; // full-range RGB input
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
        yuv.YCbCr_Matrix = 1; yuv.Nominal_Range = 1; // BT.709, 16-235
        video_context->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &rgb);
        video_context->VideoProcessorSetOutputColorSpace(processor.Get(), &yuv);
    }
    bool update(UINT timeout_ms) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication->AcquireNextFrame(timeout_ms, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        check(hr, "acquire desktop frame (display change/lock requires restarting)");
        FrameRelease release{duplication.Get()};
        ComPtr<ID3D11Texture2D> texture;
        check(resource.As(&texture), "read desktop texture");
        context->CopyResource(desktop.Get(), texture.Get());
        if (info.AccumulatedFrames > 1) coalesced += info.AccumulatedFrames - 1;
        have_frame = true;
        return true;
    }
    void convert(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->GetBufferByIndex(0, &buffer), "get pooled sample buffer");
        ComPtr<IMFDXGIBuffer> surface;
        check(buffer.As(&surface), "get pooled DXGI surface");
        ComPtr<ID3D11Texture2D> texture;
        check(surface->GetResource(IID_PPV_ARGS(&texture)), "get pooled NV12 texture");
        UINT subresource = 0;
        check(surface->GetSubresourceIndex(&subresource), "get pooled texture subresource");
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = subresource % desc.MipLevels;
        view.Texture2DArray.FirstArraySlice = subresource / desc.MipLevels;
        view.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11VideoProcessorOutputView> output;
        check(video_device->CreateVideoProcessorOutputView(texture.Get(), enumerator.Get(), &view, &output), "create NV12 output view");
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE; stream.pInputSurface = input_view.Get();
        check(video_context->VideoProcessorBlt(processor.Get(), output.Get(), 0, 1, &stream), "GPU scale and convert BGRA to NV12");
        context->Flush(); // submit GPU work before the encoder uses this surface
        ComPtr<IMF2DBuffer> buffer2d;
        check(buffer.As(&buffer2d), "get 2D buffer length");
        DWORD length = 0;
        check(buffer2d->GetContiguousLength(&length), "get contiguous NV12 length");
        check(buffer->SetCurrentLength(length), "set NV12 payload length");
    }
};

} // namespace clipping::windows
