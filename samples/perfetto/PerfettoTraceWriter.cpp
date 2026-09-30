// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <perfetto.h>

#include <windows.h>
#include <tlhelp32.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PerfettoCategories.h"
#include "PerfettoTraceWriter.h"

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace
{
    std::string Narrow(const wchar_t* w)
    {
        if (w == nullptr || *w == L'\0')
            return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1)
            return {};
        std::string s(static_cast<size_t>(len), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr) != len)
            return {};
        s.pop_back();
        return s;
    }
}

struct PerfettoTraceWriter::Impl
{
    std::string outputPath;
    int outputFile = -1;
    bool captureAllCategories = false; // when false, only GPU Queue work + API Markers
    std::unique_ptr<perfetto::TracingSession> session;

    uint64_t nextId = 1;

    // Map every event's nanosecond timestamp onto the SDK's clock domain. The
    // first event seen anchors "now"; the rest are placed relative to it. This
    // keeps the trace internally consistent regardless of the source clock epoch.
    bool baseSet = false;
    uint64_t sdkBaseNs = 0;
    int64_t firstEventNs = 0;
    uint64_t maxPft = 0;

    // Friendly names that may arrive before the track is first emitted.
    std::unordered_map<uint64_t, std::string> hwQueueName;   // hwQueueId -> name
    std::unordered_map<uint64_t, std::string> markerName;    // apiMarkerId -> name

    std::unordered_map<uint32_t, std::string> processNames;  // processId -> image name

    // Processes with their own GPU Queue track. Engine activity covers the rest, so
    // nothing is drawn as both a command-list span and a submit-to-retire packet.
    std::unordered_set<uint32_t> detailedProcessIds;

    struct ApiQueue { int64_t beginNs; std::string type; std::string name; };
    std::unordered_map<uint64_t, ApiQueue> apiQueues;

    struct Object { int64_t beginNs; std::string typeLabel; std::string name; };
    std::unordered_map<uint64_t, Object> objects;

    // Pipelined GPU work overlaps, but slices on a single Perfetto track stack LIFO
    // and would bind ends to the wrong begins. So we pack each track's spans into
    // lanes of non-overlapping spans; they share a merge key and fold back into one
    // visual track (see LaneTrack).
    //
    // trackIndex keeps the lane uuids of two tracks apart even when they end up
    // with the same label, which two adapters with identically named engines do.
    struct TrackLanes { uint64_t trackIndex; std::vector<int64_t> laneEndNs; };
    std::unordered_map<uint64_t, TrackLanes> tracks;
    uint64_t nextTrackIndex = 0;

    // Past this many concurrent spans on one track we drop the slice rather than
    // put it on a busy lane, where its end would bind to the wrong begin. Reaching
    // it means something upstream has already gone wrong - most likely a burst of
    // packets flushed at the end of the trace, all sharing an end timestamp.
    static constexpr size_t MaxLanesPerTrack = 64;
    size_t droppedSlices = 0;

    // Separates an engine's other-process track from its own. Track keys come from
    // a counter starting at 1, so the top bit is always free.
    static constexpr uint64_t OtherProcessTrackBit = 1ull << 63;

    uint64_t ToPft(int64_t eventNs)
    {
        if (!baseSet)
        {
            baseSet = true;
            firstEventNs = eventNs;
            sdkBaseNs = static_cast<uint64_t>(perfetto::base::GetBootTimeNs().count());
        }
        int64_t t = static_cast<int64_t>(sdkBaseNs) + (eventNs - firstEventNs);
        uint64_t pft = t < 0 ? 0 : static_cast<uint64_t>(t);
        if (pft > maxPft)
            maxPft = pft;
        return pft;
    }

    static perfetto::NamedTrack Track(const std::string& label, uint64_t id)
    {
        return perfetto::NamedTrack(perfetto::DynamicString{ label.c_str() }, id, perfetto::Track());
    }

    // flowId != 0 attaches a flow id to the event; events sharing a flow id are
    // connected by an arrow in the UI (in timestamp order). Used to link an API
    // marker to the GPU work it corresponds to.
    void Slice(uint64_t trackId, const std::string& trackLabel, const std::string& sliceLabel, uint64_t begin, uint64_t end, uint64_t flowId = 0)
    {
        auto track = Track(trackLabel, trackId);
        if (flowId)
            TRACE_EVENT_BEGIN("dxtimingcapture", perfetto::DynamicString{ sliceLabel.c_str() }, track, begin, perfetto::Flow::ProcessScoped(flowId));
        else
            TRACE_EVENT_BEGIN("dxtimingcapture", perfetto::DynamicString{ sliceLabel.c_str() }, track, begin);
        TRACE_EVENT_END("dxtimingcapture", track, end);
    }

    void Instant(uint64_t trackId, const std::string& trackLabel, const std::string& name, uint64_t ts, uint64_t flowId = 0)
    {
        auto track = Track(trackLabel, trackId);
        if (flowId)
            TRACE_EVENT_INSTANT("dxtimingcapture", perfetto::DynamicString{ name.c_str() }, track, ts, perfetto::Flow::ProcessScoped(flowId));
        else
            TRACE_EVENT_INSTANT("dxtimingcapture", perfetto::DynamicString{ name.c_str() }, track, ts);
    }

    // Picks a lane on trackKey's track that is free at beginNs, or nullopt once the
    // track has run out of lanes. label must outlive the returned track, which
    // points at it rather than copying it.
    std::optional<perfetto::NamedTrack> LaneTrack(uint64_t trackKey, const std::string& label, int64_t beginNs, int64_t endNs)
    {
        auto [it, inserted] = tracks.try_emplace(trackKey);
        TrackLanes& trackLanes = it->second;
        if (inserted)
        {
            trackLanes.trackIndex = nextTrackIndex++;
        }

        // Reuse the first lane that's free by beginNs, else start a new one.
        size_t lane = trackLanes.laneEndNs.size();
        for (size_t i = 0; i < trackLanes.laneEndNs.size(); ++i)
        {
            if (trackLanes.laneEndNs[i] <= beginNs) { lane = i; break; }
        }
        bool newLane = lane == trackLanes.laneEndNs.size();
        if (newLane)
        {
            if (lane >= MaxLanesPerTrack)
            {
                ++droppedSlices;
                return std::nullopt;
            }
            trackLanes.laneEndNs.push_back(endNs);
        }
        else
        {
            trackLanes.laneEndNs[lane] = endNs;
        }

        // One track per lane (lane index -> unique uuid); the shared name and
        // merge key make Perfetto render them as a single queue track.
        auto laneTrack = perfetto::NamedTrack(
            perfetto::DynamicString{ label.c_str() },
            trackLanes.trackIndex * MaxLanesPerTrack + lane + 1,
            perfetto::Track())
            .set_sibling_merge_key(trackKey);
        if (newLane)
            perfetto::TrackEvent::SetTrackDescriptor(laneTrack, laneTrack.Serialize());

        return laneTrack;
    }

    void GpuWorkSlice(uint64_t trackKey, const std::string& queueLabel, const std::string& sliceLabel, int64_t beginNs, int64_t endNs, uint64_t flowId)
    {
        auto laneTrack = LaneTrack(trackKey, queueLabel, beginNs, endNs);
        if (!laneTrack)
            return;

        uint64_t begin = ToPft(beginNs);
        uint64_t end = ToPft(endNs);
        if (flowId)
            TRACE_EVENT_BEGIN("dxtimingcapture", perfetto::DynamicString{ sliceLabel.c_str() }, *laneTrack, begin, perfetto::Flow::ProcessScoped(flowId));
        else
            TRACE_EVENT_BEGIN("dxtimingcapture", perfetto::DynamicString{ sliceLabel.c_str() }, *laneTrack, begin);
        TRACE_EVENT_END("dxtimingcapture", *laneTrack, end);
    }

    // hwQueueId is 0 when the library could not work out which engine ran a packet.
    std::string QueueLabel(uint64_t hwQueueId)
    {
        auto it = hwQueueName.find(hwQueueId);
        if (it != hwQueueName.end())
        {
            return "GPU Queue: " + it->second;
        }

        if (hwQueueId == 0)
        {
            return "GPU Queue: unidentified engine";
        }

        return "GPU Queue #" + std::to_string(hwQueueId);
    }

    static std::string ImageNameFromSnapshot(uint32_t processId)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        std::string imageName;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        {
            if (entry.th32ProcessID == processId)
            {
                imageName = Narrow(entry.szExeFile);
                break;
            }
        }

        CloseHandle(snapshot);
        return imageName;
    }

    // Cached because the lookup stops working once the process exits.
    const std::string& ProcessLabel(uint32_t processId)
    {
        auto existing = processNames.find(processId);
        if (existing != processNames.end())
        {
            return existing->second;
        }

        std::string label;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (process != nullptr)
        {
            wchar_t imagePath[MAX_PATH]{};
            DWORD pathLengthInCharacters = ARRAYSIZE(imagePath);
            if (QueryFullProcessImageNameW(process, 0, imagePath, &pathLengthInCharacters))
            {
                std::string fullPath = Narrow(imagePath);
                size_t lastSeparator = fullPath.find_last_of('\\');
                label = lastSeparator == std::string::npos ? fullPath : fullPath.substr(lastSeparator + 1);
            }
            CloseHandle(process);
        }

        // OpenProcess is denied for protected processes such as dwm and csrss, but the
        // Toolhelp snapshot reports their image names without needing a handle.
        if (label.empty())
        {
            label = ImageNameFromSnapshot(processId);
        }

        if (label.empty())
        {
            label = "PID " + std::to_string(processId);
        }
        else
        {
            label += " (" + std::to_string(processId) + ")";
        }

        return processNames.emplace(processId, std::move(label)).first->second;
    }

    std::string GpuQueueLabel(uint32_t processId, uint64_t hwQueueId)
    {
        std::string label = QueueLabel(hwQueueId);

        if (processId != 0)
        {
            label += " - " + ProcessLabel(processId);
        }

        return label;
    }
};

PerfettoTraceWriter::PerfettoTraceWriter(std::string outputPath, bool captureAllCategories)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->outputPath = std::move(outputPath);
    m_impl->captureAllCategories = captureAllCategories;

    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::TrackEvent::Register();

    perfetto::TraceConfig cfg;
    // Staging only - perfetto drains this into the output file as it fills. Buffering a
    // whole trace here would wrap and evict the track descriptors written at the start.
    cfg.add_buffers()->set_size_kb(64 * 1024);
    cfg.set_write_into_file(true);
    cfg.set_file_write_period_ms(250);
    // Track events intern their event/track names into per-sequence incremental
    // state. With a ring buffer, once the capture exceeds the buffer size the old
    // packets that defined those interned names get overwritten, and later packets
    // referencing them are dropped wholesale (reported as
    // traced_buf_incremental_sequences_dropped). Periodically clearing and
    // re-emitting incremental state gives the reader a re-sync point after a wrap.
    // https://perfetto.dev/docs/concepts/buffers#incremental-state-in-trace-packets
    cfg.mutable_incremental_state_config()->set_clear_period_ms(5000);
    auto* ds = cfg.add_data_sources()->mutable_config();
    ds->set_name("track_event");
    perfetto::protos::gen::TrackEventConfig te;
    te.add_enabled_categories("dxtimingcapture");
    // Our events carry their own (out-of-order) timestamps from ETW. Absolute
    // timestamps let the trace processor sort them; the default incremental
    // (delta) encoding would abort on a timestamp that moves backwards.
    te.set_disable_incremental_timestamps(true);
    ds->set_track_event_config_raw(te.SerializeAsString());

    if (_sopen_s(
        &m_impl->outputFile,
        m_impl->outputPath.c_str(),
        _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
        _SH_DENYWR,
        _S_IREAD | _S_IWRITE) != 0)
    {
        std::fprintf(stderr, "Failed to open %s for writing\n", m_impl->outputPath.c_str());
        m_impl->outputFile = -1;
    }

    // Writing to a file descriptor asserts unless the backend is named here too;
    // args.backends alone is not enough.
    m_impl->session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
    m_impl->session->Setup(cfg, m_impl->outputFile);
    // Setup dups the descriptor, so ours is no longer needed.
    if (m_impl->outputFile != -1)
    {
        _close(m_impl->outputFile);
        m_impl->outputFile = -1;
    }
    m_impl->session->StartBlocking();
}

PerfettoTraceWriter::~PerfettoTraceWriter() = default;

uint64_t PerfettoTraceWriter::NextId()
{
    return m_impl->nextId++;
}

void PerfettoTraceWriter::NameHwQueue(uint64_t hwQueueId, const wchar_t* name)
{
    std::string n = Narrow(name);
    if (!n.empty())
        m_impl->hwQueueName[hwQueueId] = std::move(n);
}

void PerfettoTraceWriter::EmitGpuWork(uint32_t processId, uint64_t apiQueueId, uint64_t hwQueueId, uint64_t apiMarkerId, int64_t beginNs, int64_t endNs)
{
    std::string sliceLabel = "GPU work";
    auto itMarker = m_impl->markerName.find(apiMarkerId);
    if (itMarker != m_impl->markerName.end() && !itMarker->second.empty())
        sliceLabel = itMarker->second;

    m_impl->detailedProcessIds.insert(processId);

    // Ids all come from one counter, so the engine fallback can't collide with a queue key.
    uint64_t trackKey = apiQueueId != 0 ? apiQueueId : hwQueueId;

    // Link this GPU work to its API marker with a flow arrow (apiMarkerId is the
    // same id produced in OnApiMarker / RegisterApiMarker). 0 means no marker.
    m_impl->GpuWorkSlice(trackKey, m_impl->GpuQueueLabel(processId, hwQueueId), sliceLabel, beginNs, endNs, apiMarkerId);
}

void PerfettoTraceWriter::MarkProcessDetailed(uint32_t processId)
{
    m_impl->detailedProcessIds.insert(processId);
}

void PerfettoTraceWriter::EmitOtherProcessGpuWork(uint32_t processId, uint64_t hardwareCommandQueueId, int64_t beginNs, int64_t endNs, bool hardwareScheduled)
{
    // Drawn in detail on its own track already.
    if (m_impl->detailedProcessIds.count(processId) != 0)
        return;

    // Named off the queue's own track so the two sort next to each other, and
    // keyed apart from it so they stay separate tracks.
    std::string trackLabel = m_impl->QueueLabel(hardwareCommandQueueId) + " (other/unknown processes)";
    std::string sliceLabel = processId != 0 ? m_impl->ProcessLabel(processId) : "unattributed";

    auto laneTrack = m_impl->LaneTrack(hardwareCommandQueueId | Impl::OtherProcessTrackBit, trackLabel, beginNs, endNs);
    if (!laneTrack)
        return;

    TRACE_EVENT_BEGIN(
        "dxtimingcapture",
        perfetto::DynamicString{ sliceLabel.c_str() },
        *laneTrack,
        m_impl->ToPft(beginNs),
        "pid", processId,
        "scheduler", hardwareScheduled ? "hardware" : "legacy");
    TRACE_EVENT_END("dxtimingcapture", *laneTrack, m_impl->ToPft(endNs));
}

void PerfettoTraceWriter::RegisterApiMarker(uint64_t apiMarkerId, const wchar_t* name, int64_t ts)
{
    std::string n = Narrow(name);
    m_impl->markerName[apiMarkerId] = n;
    // Same flowId as the matching GPU work, so the UI draws an arrow between them.
    m_impl->Instant(/*trackId*/ 1, "API Markers", n.empty() ? "marker" : n, m_impl->ToPft(ts), apiMarkerId);
}

void PerfettoTraceWriter::DefineApiQueue(uint64_t apiQueueId, int64_t beginNs, const wchar_t* type)
{
    if (!m_impl->captureAllCategories) return;
    m_impl->apiQueues[apiQueueId] = Impl::ApiQueue{ beginNs, Narrow(type), {} };
}

void PerfettoTraceWriter::NameApiQueue(uint64_t apiQueueId, const wchar_t* name)
{
    auto it = m_impl->apiQueues.find(apiQueueId);
    if (it != m_impl->apiQueues.end())
        it->second.name = Narrow(name);
}

void PerfettoTraceWriter::EndApiQueue(uint64_t apiQueueId, int64_t endNs)
{
    auto it = m_impl->apiQueues.find(apiQueueId);
    if (it == m_impl->apiQueues.end())
        return;

    const Impl::ApiQueue& q = it->second;
    std::string label = !q.name.empty() ? q.name
        : (!q.type.empty() ? q.type + " Queue" : "Command Queue #" + std::to_string(apiQueueId));

    m_impl->Slice(apiQueueId, label, label, m_impl->ToPft(q.beginNs), m_impl->ToPft(endNs));
    m_impl->apiQueues.erase(it);
}

void PerfettoTraceWriter::ObjectCreated(uint64_t objectId, const char* typeTrack, int64_t ts, std::wstring label)
{
    (void)typeTrack;
    if (!m_impl->captureAllCategories) return;
    m_impl->objects[objectId] = Impl::Object{ ts, Narrow(label.c_str()), {} };
}

void PerfettoTraceWriter::ObjectNamed(uint64_t objectId, const wchar_t* name)
{
    auto it = m_impl->objects.find(objectId);
    if (it != m_impl->objects.end())
        it->second.name = Narrow(name);
}

void PerfettoTraceWriter::ObjectDestroyed(uint64_t objectId, int64_t ts)
{
    auto it = m_impl->objects.find(objectId);
    if (it == m_impl->objects.end())
        return;

    const Impl::Object& o = it->second;
    std::string label = o.name.empty() ? o.typeLabel : o.typeLabel + ": " + o.name;
    m_impl->Slice(objectId, label, label, m_impl->ToPft(o.beginNs), m_impl->ToPft(ts));
    m_impl->objects.erase(it);
}

void PerfettoTraceWriter::EmitPsoCompile(uint32_t threadId, int64_t startNs, int64_t endNs)
{
    if (!m_impl->captureAllCategories) return;
    std::string label = "PSO Compiles (thread " + std::to_string(threadId) + ")";
    // Thread id keys the track so concurrent compiles land on separate tracks.
    m_impl->Slice(0x50000000ull | threadId, label, "PSO compile", m_impl->ToPft(startNs), m_impl->ToPft(endNs));
}

void PerfettoTraceWriter::Finish()
{
    // Close any slices that never saw their end event: queues still open and
    // objects never destroyed, ended at the last timestamp we observed.
    uint64_t end = m_impl->maxPft;
    for (auto& [id, q] : m_impl->apiQueues)
    {
        std::string label = !q.name.empty() ? q.name
            : (!q.type.empty() ? q.type + " Queue" : "Command Queue #" + std::to_string(id));
        m_impl->Slice(id, label, label, m_impl->ToPft(q.beginNs), end);
    }
    for (auto& [id, o] : m_impl->objects)
    {
        std::string label = o.name.empty() ? o.typeLabel : o.typeLabel + ": " + o.name;
        m_impl->Slice(id, label, label, m_impl->ToPft(o.beginNs), end);
    }
    m_impl->apiQueues.clear();
    m_impl->objects.clear();

    perfetto::TrackEvent::Flush();
    m_impl->session->StopBlocking();

    std::error_code sizeError;
    const auto writtenBytes = std::filesystem::file_size(m_impl->outputPath, sizeError);

    std::printf("Wrote %llu bytes to %s\n",
        sizeError ? 0ull : static_cast<unsigned long long>(writtenBytes),
        m_impl->outputPath.c_str());    if (m_impl->droppedSlices)
        std::printf("Dropped %zu slices that had nowhere non-overlapping to go\n", m_impl->droppedSlices);
}
