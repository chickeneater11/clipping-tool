#include "capture_support.hpp"
using namespace clipping::windows;
namespace {
void verify_hardware(IMFSinkWriter* writer, DWORD stream) {
    ComPtr<IMFSinkWriterEx> extended;
    check(writer->QueryInterface(IID_PPV_ARGS(&extended)), "inspect selected encoder");
    // With NV12 input, this path should contain the encoder without converters.
    for (DWORD i = 0; i < 16; ++i) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        const HRESULT hr = extended->GetTransformForStream(stream, i, &category, &transform);
        if (FAILED(hr)) break;
        if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
        ComPtr<IMFAttributes> attributes;
        check(transform->GetAttributes(&attributes), "inspect encoder attributes");
        UINT32 length = 0, aware = 0;
        check(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length), "refuse encoder without hardware identity");
        check(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware), "check encoder D3D11 awareness");
        if (!length || !aware) throw Failure{"refuse non-hardware/non-D3D11 encoder", E_NOTIMPL};
        std::cout << "Selected encoder: hardware MFT, D3D11-aware (verified before recording).\n";
        return;
    }
    throw Failure{"could not verify a hardware video encoder; no software fallback allowed", E_NOTIMPL};
}

// Each diagnostic uses a fresh in-memory stream and sink. No recording or
// encoding occurs, and a failed attempt cannot contaminate the next attempt.
void diagnose_writer_settings(IMFDXGIDeviceManager* manager) {
    struct Case { const char* name; bool gpu, hardware, converters_off, low_latency; };
    const Case cases[] = {
        {"no attributes", false, false, false, false},
        {"GPU manager only", true, false, false, false},
        {"hardware flag only", false, true, false, false},
        {"GPU + hardware (recording settings)", true, true, false, false},
        {"GPU + hardware + converters disabled", true, true, true, false},
        {"GPU + hardware + low latency", true, true, false, true}
    };
    for (const auto& test : cases) {
        try {
            ComPtr<IStream> memory;
            check(CreateStreamOnHGlobal(nullptr, TRUE, &memory), "diagnostic memory stream");
            ComPtr<IMFByteStream> bytes;
            check(MFCreateMFByteStreamOnStream(memory.Get(), &bytes), "diagnostic byte stream");
            auto type = video_type(true);
            OwnedMediaSink sink;
            check(MFCreateMPEG4MediaSink(bytes.Get(), type.Get(), nullptr, &sink.sink), "diagnostic MP4 sink");
            ComPtr<IMFAttributes> settings;
            check(MFCreateAttributes(&settings, 4), "diagnostic attributes");
            if (test.gpu) check(settings->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager), "diagnostic GPU setting");
            if (test.hardware) check(settings->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "diagnostic hardware setting");
            if (test.converters_off) check(settings->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE), "diagnostic converter setting");
            if (test.low_latency) check(settings->SetUINT32(MF_LOW_LATENCY, TRUE), "diagnostic latency setting");
            ComPtr<IMFSinkWriter> candidate;
            const HRESULT hr = MFCreateSinkWriterFromMediaSink(sink.sink.Get(),
                test.gpu || test.hardware ? settings.Get() : nullptr, &candidate);
            std::cout << "Writer check [" << test.name << "]: 0x" << std::hex
                      << static_cast<unsigned long>(hr) << std::dec << "\n";
        } catch (const Failure& failure) {
            std::cout << "Writer check [" << test.name << "] setup failed at " << failure.stage
                      << ": 0x" << std::hex << static_cast<unsigned long>(failure.hr) << std::dec << "\n";
        }
    }
}

int record(const wchar_t* path, UINT seconds, UINT adapter, UINT output) {
    Runtime runtime; runtime.start();
    Capture capture; capture.start(adapter, output);
    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> manager;
    check(MFCreateDXGIDeviceManager(&token, &manager), "create device manager");
    check(manager->ResetDevice(capture.device.Get(), token), "attach capture device manager");
    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(&attributes, 2), "create writer settings");
    check(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager.Get()), "set writer GPU manager");
    check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "enable hardware encoder selection");
    ComPtr<IMFByteStream> file;
    // Archive media sinks need a readable, writable, seekable byte stream.
    // Write-only creation can make the MP4 sink reject setup with E_INVALIDARG.
    check(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, path, &file), "create output file (choose a new filename if it exists)");
    DWORD capabilities = 0;
    check(file->GetCapabilities(&capabilities), "query output stream capabilities");
    constexpr DWORD required = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_WRITABLE | MFBYTESTREAM_IS_SEEKABLE;
    if ((capabilities & required) != required)
        throw Failure{"MP4 output must be readable, writable, and seekable", E_INVALIDARG};
    std::cout << "Output stream capabilities: 0x" << std::hex << capabilities << std::dec << "\n";
    auto encoded = video_type(true), raw = video_type(false);
    OwnedMediaSink media_sink;
    check(MFCreateMPEG4MediaSink(file.Get(), encoded.Get(), nullptr, &media_sink.sink),
          "create explicit H.264 MP4 media sink");
    std::cout << "Explicit MP4 media sink created.\n";
    // This sink has a fixed video stream; do not call AddStream on the writer.
    ComPtr<IMFStreamSink> video_stream;
    check(media_sink.sink->GetStreamSinkByIndex(0, &video_stream), "get MP4 video stream");
    DWORD sink_stream_id = 0;
    check(video_stream->GetIdentifier(&sink_stream_id), "get MP4 video stream identifier");
    // Sink-writer APIs take a zero-based index, NOT the sink's track identifier.
    constexpr DWORD stream = 0;
    std::cout << "Video mapping: sink identifier=" << sink_stream_id
              << ", writer index=" << stream << "\n";
    ComPtr<IMFSinkWriter> writer;
    const HRESULT writer_result = MFCreateSinkWriterFromMediaSink(media_sink.sink.Get(), attributes.Get(), &writer);
    if (FAILED(writer_result)) {
        std::cout << "Minimal writer setup failed; running isolated settings checks (no recording).\n";
        diagnose_writer_settings(manager.Get());
        check(writer_result, "create writer with GPU manager + hardware flag only");
    }
    std::cout << "Writer created from explicit MP4 sink.\n";
    check(writer->SetInputMediaType(stream, raw.Get(), nullptr), "configure NV12 encoder input");
    verify_hardware(writer.Get(), stream);
    ComPtr<IMFVideoSampleAllocatorEx> pool;
    check(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&pool)), "create video sample pool");
    check(pool->SetDirectXManager(manager.Get()), "set sample pool GPU manager");
    ComPtr<IMFAttributes> pool_attributes;
    check(MFCreateAttributes(&pool_attributes, 2), "create pool settings");
    check(pool_attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET), "set pool texture bindings");
    check(pool_attributes->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT), "set pool texture usage");
    check(pool->InitializeSampleAllocatorEx(2, 6, pool_attributes.Get(), raw.Get()), "initialize bounded NV12 sample pool");
    const auto first_deadline = Clock::now() + std::chrono::seconds(5);
    while (!capture.have_frame && Clock::now() < first_deadline) capture.update(100);
    if (!capture.have_frame) throw Failure{"no desktop frame within five seconds", HRESULT_FROM_WIN32(WAIT_TIMEOUT)};
    check(writer->BeginWriting(), "begin MP4 recording");
    std::cout << "Recording " << seconds << " seconds at 1920x1080 / 60 fps / 12 Mbps, video only.\n";
    unsigned long long submitted = 0, repeats = 0, missed = 0, pool_drops = 0;
    long long slot = 0;
    double max_write_ms = 0;
    const auto start = Clock::now();
    const long long total = static_cast<long long>(seconds) * fps;
    while (slot < total) {
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(slot * 1'000'000'000 / fps));
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        const long long current = elapsed * fps / 1'000'000'000;
        if (current > slot) { missed += static_cast<unsigned long long>(std::min(current, total) - slot); slot = current; }
        if (slot >= total) break;
        const bool updated = capture.update(0);
        ComPtr<IMFSample> sample;
        const HRESULT allocated = pool->AllocateSample(&sample);
        if (allocated == MF_E_SAMPLEALLOCATOR_EMPTY) { ++pool_drops; ++slot; continue; }
        check(allocated, "allocate bounded GPU sample");
        capture.convert(sample.Get());
        const LONGLONG pts = slot * 10'000'000 / fps;
        check(sample->SetSampleTime(pts), "set sample timestamp");
        check(sample->SetSampleDuration((slot + 1) * 10'000'000 / fps - pts), "set sample duration");
        const auto before = Clock::now();
        check(writer->WriteSample(stream, sample.Get()), "encode and write video sample");
        max_write_ms = std::max(max_write_ms, std::chrono::duration<double, std::milli>(Clock::now() - before).count());
        ++submitted; if (!updated) ++repeats;
        ++slot;
    }
    const auto recording_end = Clock::now();
    if (!submitted) throw Failure{"no frames submitted", E_FAIL};
    check(writer->Finalize(), "finalize MP4 (flush encoder and write container metadata)");
    std::cout << "MP4 finalized.\nFrames submitted: " << submitted
              << "\nRepeated desktop frames (no new update): " << repeats
              << "\nMissed scheduled slots: " << missed << "\nSample-pool drops: " << pool_drops
              << "\nCoalesced desktop updates: " << capture.coalesced
              << "\nRecording wall time (s): " << std::chrono::duration<double>(recording_end - start).count()
              << "\nFinalization time (s): " << std::chrono::duration<double>(Clock::now() - recording_end).count()
              << "\nLongest WriteSample call (ms): " << max_write_ms
              << "\nOpen the MP4 to verify playback. These counters are not game FPS measurements.\n";
    return 0;
}
UINT number(const wchar_t* value, UINT minimum, UINT maximum) {
    const std::wstring text(value);
    if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::invalid_argument("arguments must be nonnegative integers");
    const auto parsed = std::stoul(text);
    if (parsed < minimum || parsed > maximum) throw std::invalid_argument("numeric argument outside allowed range");
    return static_cast<UINT>(parsed);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    std::cout << "Clipping recorder: stream-index-v4\n";
    if (argc > 5 || (argc > 1 && (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h"))) {
        std::cout << "Usage: clipping_record_test [new-file.mp4] [seconds 1-60] [adapter 0-31] [output 0-31]\n"
                     "Defaults: clip-test.mp4, 10 seconds, adapter 0, output 0.\n"
                     "Records the entire selected display, without audio. Existing files are never overwritten.\n";
        return argc > 5 ? 1 : 0;
    }
    try {
        return record(argc > 1 ? argv[1] : L"clip-test.mp4", argc > 2 ? number(argv[2], 1, 60) : 10,
                      argc > 3 ? number(argv[3], 0, 31) : 0, argc > 4 ? number(argv[4], 0, 31) : 0);
    } catch (const Failure& failure) {
        std::cerr << "Recording failed at: " << failure.stage << " (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(failure.hr) << std::dec << ").\n"
                  << "Any newly created output may be incomplete. Use a new filename on retry.\n";
        return 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
