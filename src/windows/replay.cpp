#include "capture_support.hpp"
#include "clipping/replay_buffer.hpp"
#include "clipping/h264.hpp"
#include <strmif.h>
#include <codecapi.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <limits>

using namespace clipping::windows;
namespace {
constexpr std::size_t payload_budget = 64 * 1024 * 1024;
constexpr DWORD packet_limit = 8 * 1024 * 1024;
std::atomic_bool stopping{false};
BOOL WINAPI console_control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) { stopping.store(true); return TRUE; }
    return FALSE;
}
struct Controls {
    bool hotkey = false, handler = false;
    void start() {
        if (!RegisterHotKey(nullptr, 1, MOD_NOREPEAT, VK_F8))
            throw Failure{"register F8 (another app may already own it)", HRESULT_FROM_WIN32(GetLastError())};
        hotkey = true;
        if (!SetConsoleCtrlHandler(console_control, TRUE))
            throw Failure{"install Ctrl+C handler", HRESULT_FROM_WIN32(GetLastError())};
        handler = true;
    }
    ~Controls() { if (hotkey) UnregisterHotKey(nullptr, 1); if (handler) SetConsoleCtrlHandler(console_control, FALSE); }
};
struct Activations {
    IMFActivate** items = nullptr;
    UINT32 count = 0;
    ~Activations() { for (UINT32 i = 0; i < count; ++i) items[i]->Release(); CoTaskMemFree(items); }
};
struct LockedBuffer {
    IMFMediaBuffer* buffer;
    BYTE* bytes = nullptr;
    DWORD length = 0;
    explicit LockedBuffer(IMFMediaBuffer* value) : buffer(value) {
        check(buffer->Lock(&bytes, nullptr, &length), "lock encoded packet");
    }
    ~LockedBuffer() { buffer->Unlock(); }
};
struct Encoder {
    ComPtr<IMFActivate> activation;
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaEventGenerator> events;
    DWORD input_id = 0, output_id = 0;
    unsigned credits = 0;
    bool drained = false;
    std::uint64_t outputs = 0, rejected = 0;
    ~Encoder() { close(); }
    void close() {
        if (transform) {
            transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        events.Reset(); transform.Reset();
        if (activation) activation->ShutdownObject();
        activation.Reset(); credits = 0;
    }
    void start(IMFDXGIDeviceManager* manager) {
        Activations found;
        MFT_REGISTER_TYPE_INFO filter{MFMediaType_Video, MFVideoFormat_H264};
        check(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE, nullptr, &filter,
                       &found.items, &found.count), "enumerate hardware encoders");
        for (UINT32 i = 0; i < found.count; ++i) {
            try {
                activation = found.items[i];
                check(activation->ActivateObject(IID_PPV_ARGS(&transform)), "activate encoder");
                ComPtr<IMFAttributes> attributes;
                check(transform->GetAttributes(&attributes), "encoder attributes");
                check(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "unlock asynchronous encoder");
                UINT32 aware = 0;
                check(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware), "query GPU input support");
                if (!aware) throw Failure{"encoder is not D3D11-aware", E_NOINTERFACE};
                check(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager)), "attach capture GPU to encoder");
                DWORD inputs = 0, outputs_count = 0;
                check(transform->GetStreamCount(&inputs, &outputs_count), "encoder stream count");
                if (inputs != 1 || outputs_count != 1) throw Failure{"single-stream encoder required", E_NOTIMPL};
                input_id = output_id = 0;
                const HRESULT ids = transform->GetStreamIDs(1, &input_id, 1, &output_id);
                if (ids != E_NOTIMPL) check(ids, "encoder stream identifiers");
                auto output = video_type(true);
                // Baseline H.264 excludes B pictures. Also enforce no reordering
                // when accepting output timestamps below.
                check(output->SetUINT32(MF_MT_MPEG2_PROFILE, 66), "request baseline H.264");
                check(transform->SetOutputType(output_id, output.Get(), 0), "set hardware H.264 output");
                auto input = video_type(false);
                check(transform->SetInputType(input_id, input.Get(), 0), "set hardware NV12 input");
                ComPtr<ICodecAPI> codec;
                check(transform.As(&codec), "encoder codec settings");
                VARIANT value; VariantInit(&value); value.vt = VT_UI4;
                value.ulVal = fps;
                check(codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &value), "set one-second GOP");
                value.ulVal = 0;
                check(codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value), "disable B pictures");
                check(transform.As(&events), "encoder event interface");
                check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "begin encoder streaming");
                check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "start encoder stream");
                std::cout << "Direct hardware encoder ready; baseline H.264, no B frames, GOP 60.\n";
                return;
            } catch (const Failure& failure) {
                std::cerr << "Encoder candidate " << i << " rejected at " << failure.stage << ": 0x"
                          << std::hex << static_cast<unsigned long>(failure.hr) << std::dec << '\n';
                close();
            }
        }
        throw Failure{"no hardware encoder accepted the replay configuration", E_FAIL};
    }
    void receive(clipping::ReplayBuffer& replay, unsigned renegotiations = 0) {
        MFT_OUTPUT_STREAM_INFO info{};
        check(transform->GetOutputStreamInfo(output_id, &info), "encoder output allocation requirements");
        ComPtr<IMFSample> provided;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            if (!info.cbSize || info.cbSize > packet_limit || info.cbAlignment > packet_limit)
                throw Failure{"encoder requires an oversized output buffer", E_OUTOFMEMORY};
            check(MFCreateSample(&provided), "allocate encoder output sample");
            ComPtr<IMFMediaBuffer> storage;
            check(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &storage), "allocate encoded output buffer");
            check(provided->AddBuffer(storage.Get()), "attach output storage");
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = output_id; output.pSample = provided.Get();
        DWORD status = 0;
        const HRESULT result = transform->ProcessOutput(0, 1, &output, &status);
        ComPtr<IMFCollection> event_collection; event_collection.Attach(output.pEvents);
        ComPtr<IMFSample> sample;
        if (output.pSample == provided.Get()) sample = provided;
        else sample.Attach(output.pSample);
        // Some encoders announce their finalized format with the first output.
        // Allow one initial renegotiation, but never mix codec configurations.
        if (result == MF_E_TRANSFORM_STREAM_CHANGE && outputs == 0 && renegotiations == 0) {
            ComPtr<IMFMediaType> updated;
            check(transform->GetOutputAvailableType(output_id, 0, &updated), "query initial encoder format change");
            GUID subtype{};
            check(updated->GetGUID(MF_MT_SUBTYPE, &subtype), "read changed encoder subtype");
            UINT32 changed_width = 0, changed_height = 0;
            check(MFGetAttributeSize(updated.Get(), MF_MT_FRAME_SIZE, &changed_width, &changed_height), "read changed encoder dimensions");
            if (subtype != MFVideoFormat_H264 || changed_width != width || changed_height != height)
                throw Failure{"unexpected encoder format change", MF_E_INVALIDMEDIATYPE};
            check(transform->SetOutputType(output_id, updated.Get(), 0), "accept initial encoder format");
            receive(replay, 1);
            return;
        }
        check(result, "read encoded output (midstream format changes require restart)");
        if (!sample) return;
        LONGLONG pts = 0;
        check(sample->GetSampleTime(&pts), "read encoded timestamp");
        if (pts < 0) throw Failure{"negative encoder timestamp", E_UNEXPECTED};
        DWORD size = 0;
        check(sample->GetTotalLength(&size), "measure encoded packet");
        if (!size) return;
        if (size > packet_limit) throw Failure{"encoded packet exceeded 8 MiB safety limit", E_OUTOFMEMORY};
        ComPtr<IMFMediaBuffer> data;
        check(sample->ConvertToContiguousBuffer(&data), "get encoded access unit");
        LockedBuffer locked(data.Get());
        if (locked.length != size) throw Failure{"encoded packet length changed", E_UNEXPECTED};
        clipping::Packet packet{static_cast<std::uint64_t>(pts / 10), false, std::vector<std::byte>(size)};
        std::memcpy(packet.data.data(), locked.bytes, size);
        const auto nal = clipping::inspect_h264(packet.data);
        if (!nal.annex_b) throw Failure{"encoder output is not supported Annex B H.264", E_NOTIMPL};
        packet.keyframe = nal.idr;
        const auto accepted = replay.push(std::move(packet));
        if (accepted == clipping::PushResult::invalid)
            throw Failure{"encoder output timestamps are not strictly increasing", E_UNEXPECTED};
        if (accepted != clipping::PushResult::stored) ++rejected;
        ++outputs;
    }
    void pump(clipping::ReplayBuffer& replay) {
        // Bound one pump pass so messages/capture cannot starve.
        for (unsigned i = 0; i < 64; ++i) {
            ComPtr<IMFMediaEvent> event;
            const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) return;
            check(hr, "poll encoder events");
            HRESULT event_status = S_OK;
            check(event->GetStatus(&event_status), "read encoder event status");
            check(event_status, "asynchronous encoder failure");
            MediaEventType type = MEUnknown;
            check(event->GetType(&type), "read encoder event type");
            if (type == METransformNeedInput) {
                UINT32 id = 0;
                check(event->GetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, &id), "read requested stream ID");
                if (id != input_id || credits >= 64) throw Failure{"unexpected encoder input credits", E_UNEXPECTED};
                ++credits;
            } else if (type == METransformHaveOutput) receive(replay);
            else if (type == METransformDrainComplete) drained = true;
        }
    }
    void submit(IMFSample* sample) {
        if (!credits) throw Failure{"input submitted without encoder readiness", E_UNEXPECTED};
        check(transform->ProcessInput(input_id, sample, 0), "submit GPU frame to direct encoder");
        --credits;
    }
};

void save_replay(const std::filesystem::path& folder, Encoder& encoder, const clipping::ReplayBuffer& replay) {
    if (replay.packets().empty()) { std::cout << "Buffer is waiting for an IDR; no clip saved yet.\n"; return; }
    auto started = Clock::now();
    ComPtr<IMFMediaType> type;
    check(encoder.transform->GetOutputCurrentType(encoder.output_id, &type), "read H.264 export format");
    UINT32 header_size = 0;
    const auto& packets = replay.packets();
    const auto first = clipping::inspect_h264(packets.front().data);
    if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &header_size)) || !header_size) {
        if (!first.sps || !first.pps)
            throw Failure{"cannot export without codec headers or SPS/PPS in the first IDR", MF_E_INVALIDMEDIATYPE};
    }
    // CREATE_NEW-style behavior, including when clocks repeat or another process
    // saves concurrently. No existing clips are overwritten.
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    ComPtr<IMFByteStream> file;
    std::filesystem::path path;
    HRESULT opened = E_FAIL;
    for (unsigned suffix = 0; suffix < 100; ++suffix) {
        path = folder / (L"clip-" + std::to_wstring(stamp) + L"-" + std::to_wstring(suffix) + L".mp4");
        opened = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, path.c_str(), &file);
        if (opened != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) && opened != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) break;
    }
    check(opened, "create new replay MP4");
    OwnedMediaSink sink;
    check(MFCreateMPEG4MediaSink(file.Get(), type.Get(), nullptr, &sink.sink), "create replay MP4 sink");
    ComPtr<IMFSinkWriter> writer;
    check(MFCreateSinkWriterFromMediaSink(sink.sink.Get(), nullptr, &writer), "create replay mux writer");
    // Both types are identical compressed H.264: passthrough, not re-encoding.
    check(writer->SetInputMediaType(0, type.Get(), nullptr), "set compressed H.264 passthrough");
    check(writer->BeginWriting(), "begin replay MP4");
    const auto origin = packets.front().pts_us;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto& packet = packets[i];
        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample), "allocate mux sample");
        ComPtr<IMFMediaBuffer> data;
        check(MFCreateMemoryBuffer(static_cast<DWORD>(packet.data.size()), &data), "allocate mux packet");
        {
            LockedBuffer locked(data.Get());
            std::memcpy(locked.bytes, packet.data.data(), packet.data.size());
        }
        check(data->SetCurrentLength(static_cast<DWORD>(packet.data.size())), "set mux packet size");
        check(sample->AddBuffer(data.Get()), "attach mux packet");
        check(sample->SetSampleTime(static_cast<LONGLONG>((packet.pts_us - origin) * 10)), "rebase clip timestamp");
        const auto duration = i + 1 < packets.size() ? (packets[i + 1].pts_us - packet.pts_us) * 10 : 10'000'000 / fps;
        check(sample->SetSampleDuration(static_cast<LONGLONG>(duration)), "set clip packet duration");
        check(sample->SetUINT32(MFSampleExtension_CleanPoint, packet.keyframe), "set IDR marker");
        check(writer->WriteSample(0, sample.Get()), "mux replay packet");
    }
    check(writer->Finalize(), "finalize replay MP4");
    std::wcout << L"Saved " << path.wstring() << L"\n";
    std::cout << "Clip seconds: " << static_cast<double>(packets.back().pts_us - origin) / 1'000'000 + 1.0 / fps
              << "; capture pause for save (s): " << std::chrono::duration<double>(Clock::now() - started).count() << '\n';
}

int run(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    Runtime runtime; runtime.start();
    Controls controls; controls.start();
    Capture capture; capture.start(0, 0);
    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> manager;
    check(MFCreateDXGIDeviceManager(&token, &manager), "create replay device manager");
    check(manager->ResetDevice(capture.device.Get(), token), "attach replay capture device");
    ComPtr<IMFVideoSampleAllocatorEx> pool;
    check(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&pool)), "create replay GPU pool");
    check(pool->SetDirectXManager(manager.Get()), "attach pool device manager");
    ComPtr<IMFAttributes> pool_settings;
    check(MFCreateAttributes(&pool_settings, 2), "create pool attributes");
    check(pool_settings->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET), "set replay pool bindings");
    check(pool_settings->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT), "set replay pool usage");
    auto input = video_type(false);
    check(pool->InitializeSampleAllocatorEx(2, 6, pool_settings.Get(), input.Get()), "allocate six-sample GPU pool");
    Encoder encoder; encoder.start(manager.Get());
    clipping::ReplayBuffer replay({payload_budget, 3600, 30'000'000});
    const auto initial_deadline = Clock::now() + std::chrono::seconds(5);
    while (!capture.have_frame && Clock::now() < initial_deadline && !stopping.load()) capture.update(100);
    if (!capture.have_frame) throw Failure{"no initial desktop image", HRESULT_FROM_WIN32(WAIT_TIMEOUT)};
    std::cout << "Replay running: 1080p60, 12 Mbps, up to 30 seconds / 64 MiB encoded payload.\n"
                 "F8 saves recent video. Ctrl+C stops. Video only, entire display 0 on adapter 0.\n"
                 "Saving pauses capture; repeated F8 presses during a save are discarded.\n";
    const auto start = Clock::now();
    auto next_report = start + std::chrono::seconds(5);
    auto last_output = start;
    std::uint64_t last_count = 0, submitted = 0, missed = 0, busy = 0;
    long long slot = 0;
    while (!stopping.load()) {
        encoder.pump(replay);
        if (encoder.outputs != last_count) { last_count = encoder.outputs; last_output = Clock::now(); }
        if (Clock::now() - last_output > std::chrono::seconds(5))
            throw Failure{"encoder produced no output for five seconds", HRESULT_FROM_WIN32(WAIT_TIMEOUT)};
        MSG message{};
        bool save = false;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_HOTKEY && message.wParam == 1) save = true;
        }
        if (save) {
            try { save_replay(folder, encoder, replay); }
            catch (const Failure& failure) {
                std::cerr << "Save failed at " << failure.stage << ": 0x" << std::hex
                          << static_cast<unsigned long>(failure.hr) << std::dec << ". Partial file may remain.\n";
            }
            while (PeekMessageW(&message, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {}
            last_output = Clock::now(); // time spent in synchronous export isn't an encoder stall
        }
        const auto now = Clock::now();
        if (now >= next_report) {
            const auto retained = replay.packets().empty() ? 0.0 : static_cast<double>(replay.packets().back().pts_us - replay.packets().front().pts_us) / 1'000'000;
            std::cout << "Buffered " << retained << " s, " << replay.payload_bytes() / (1024.0 * 1024.0)
                      << " MiB; input=" << submitted << ", output=" << encoder.outputs
                      << ", skipped slots=" << missed << ", busy slots=" << busy
                      << ", waiting-IDR/rejected=" << encoder.rejected << '\n';
            next_report = now + std::chrono::seconds(5);
        }
        const long long current = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count() * fps / 1'000'000'000;
        if (current < slot) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        if (current > slot) missed += static_cast<std::uint64_t>(current - slot);
        slot = current + 1;
        if (!encoder.credits) { ++busy; continue; }
        ComPtr<IMFSample> sample;
        const HRESULT allocated = pool->AllocateSample(&sample);
        if (allocated == MF_E_SAMPLEALLOCATOR_EMPTY) { ++busy; continue; }
        check(allocated, "allocate replay input sample");
        capture.update(0); capture.convert(sample.Get());
        check(sample->SetSampleTime(current * 10'000'000 / fps), "set replay timestamp");
        check(sample->SetSampleDuration((current + 1) * 10'000'000 / fps - current * 10'000'000 / fps), "set replay duration");
        encoder.submit(sample.Get()); ++submitted;
    }
    // No implicit save on exit: F8 is the explicit save operation.
    check(encoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "end replay input");
    check(encoder.transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "drain replay encoder");
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!encoder.drained && Clock::now() < deadline) { encoder.pump(replay); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    std::cout << "Stopped. Drain completed: " << (encoder.drained ? "yes" : "timeout; encoder will be flushed") << '\n';
    return 0;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    std::cout << "Clipping replay: direct-h264-v1\n";
    if (argc > 2 || (argc == 2 && std::wstring(argv[1]) == L"--help")) {
        std::cout << "Usage: clipping_replay [output-folder]\nDefault: clips. F8 saves, Ctrl+C stops.\n";
        return argc > 2 ? 1 : 0;
    }
    try { return run(argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path(L"clips")); }
    catch (const Failure& failure) {
        std::cerr << "Replay failed at " << failure.stage << " (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(failure.hr) << std::dec << ").\n"; return 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
