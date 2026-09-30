// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <chrono>
#include <cstdio>
#include <memory>

#include <evntrace.h>

#include <d3d11.h>

#include <DxTimingCaptureLibrary/DxTimingCaptureLibrary.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

// The Win32 backend deliberately hides this behind #if 0 to avoid pulling in
// <windows.h>; forward-declare it ourselves so our WndProc can forward to it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "EtwConfig.h"
#include "MemoryModel.h"
#include "MemoryMapCallbacks.h"

using namespace DirectX::Etw;
using namespace memorymap;

namespace
{
    // ---- Shared state fed by the ETW consumer -------------------------------

    MemoryModel g_model;
    std::unique_ptr<DxTimingCaptureEventHandler> g_handler;

    std::wstring g_sessionName;
    TRACEHANDLE g_sessionHandle = 0;
    std::atomic<bool> g_running{ true };

    // Populates the model with synthetic objects so the visualization can be
    // seen without a GPU or ETW permissions: a video heap with two placed
    // resources nested inside it, an upload committed resource in system memory,
    // and one object whose pool isn't known yet.
    void PopulateDemoModel()
    {
        g_model.Add({ 1, ObjectKind::Heap, L"BigVideoHeap", 0x400000, 0x100000, MemorySegmentGroup::Local });
        g_model.Add({ 2, ObjectKind::PlacedResource, L"PlacedTexture", 0x180000, 0x100000, MemorySegmentGroup::Local });
        g_model.Add({ 3, ObjectKind::PlacedResource, L"PlacedBuffer", 0x80000, 0x280000, MemorySegmentGroup::Local });
        g_model.Add({ 4, ObjectKind::CommittedResource, L"UploadBuffer", 0x200000, 0x900000, MemorySegmentGroup::NonLocal });
        g_model.Add({ 5, ObjectKind::CommittedResource, L"RenderTarget", 0x400000, 0xA00000, MemorySegmentGroup::Local });
        g_model.Add({ 6, ObjectKind::CommittedResource, L"StagingBuffer", 0x100000, 0xB00000, MemorySegmentGroup::NonLocal });
        g_model.Add({ 7, ObjectKind::CommittedResource, L"PendingResource", 0x40000, 0xC00000, MemorySegmentGroup::Unknown });

        // Two 3 MB placed resources aliased at the same offset in a 6 MB heap:
        // they should overlap (stacked lanes) with the other 3 MB shown free.
        g_model.Add({ 8, ObjectKind::Heap, L"AliasHeap", 0x600000, 0x2000000, MemorySegmentGroup::Local });
        g_model.Add({ 9, ObjectKind::PlacedResource, L"AliasedA", 0x300000, 0x2000000, MemorySegmentGroup::Local });
        g_model.Add({ 10, ObjectKind::PlacedResource, L"AliasedB", 0x300000, 0x2000000, MemorySegmentGroup::Local });
    }

    // ---- ETW plumbing -------------------------------------------------------

    void WINAPI OnEvent(EVENT_RECORD* record)
    {
        // Never let an exception unwind back through ProcessTrace's C frames.
        try
        {
            g_handler->HandleEventRecord(record);
        }
        catch (...)
        {
        }
    }

    ULONG WINAPI OnBuffer(PEVENT_TRACE_LOGFILE logfile)
    {
        g_handler->ReportTraceStatistics(*logfile);
        return TRUE;
    }

    void CreateHandler(DWORD processId)
    {
        DxTimingCaptureLibraryOptions options{};
        options.TrackApiObjects = true;

        DxTimingCaptureEventCallbacks callbacks{};
        static ApiObjectSink apiObjectSink(g_model);
        static ResidencySink residencySink(g_model);
        callbacks.ApiObjectCallbacks = &apiObjectSink;
        callbacks.ResidencyEventCallbacks = &residencySink;

        g_handler = DxTimingCaptureEventHandler::Create(processId, options, callbacks);
    }

    // Live real-time capture against a running process, on its own thread.
    void LiveConsumerThread()
    {
        EVENT_TRACE_LOGFILE trace = {};
        trace.LoggerName = const_cast<LPWSTR>(g_sessionName.c_str());
        trace.ProcessTraceMode =
            PROCESS_TRACE_MODE_REAL_TIME |
            PROCESS_TRACE_MODE_EVENT_RECORD |
            PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        trace.EventRecordCallback = OnEvent;
        trace.BufferCallback = OnBuffer;

        TRACEHANDLE traceHandle = OpenTrace(&trace);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            printf("OpenTrace failed: %lu\n", GetLastError());
            return;
        }

        ProcessTrace(&traceHandle, 1, nullptr, nullptr);

        g_handler->OnDataComplete();
        CloseTrace(traceHandle);
    }

    bool StartLiveSession(DWORD processId)
    {
        g_sessionName = L"DxTimingCaptureLibrary.memorymap.session" + std::to_wstring(GetCurrentProcessId());

        CreateHandler(processId);

        EventTraceProperties properties{};
        ULONG status = ::StartTraceW(&g_sessionHandle, g_sessionName.c_str(), properties);
        if (status != ERROR_SUCCESS)
        {
            printf("StartTraceW failed: %lu (are you in the 'Performance Log Users' group?)\n", status);
            return false;
        }

        EnableD3D12Provider(g_sessionHandle, true);
        EnableDxgkProvider(g_sessionHandle, true, true);
        return true;
    }

    void StopLiveSession()
    {
        EventTraceProperties properties{};
        ControlTraceW(g_sessionHandle, g_sessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    }

    // Records the same providers StartLiveSession uses, but to an .etl on disk
    // instead of real time, so a capture can be replayed deterministically while
    // debugging. Stopped via StopLiveSession (same handle/name).
    bool StartRecordingSession(const std::wstring& etlFilePath)
    {
        g_sessionName = L"DxTimingCaptureLibrary.memorymap.record" + std::to_wstring(GetCurrentProcessId());

        EventTraceProperties properties{};
        properties->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
        properties->LogFileNameOffset = FIELD_OFFSET(EventTraceProperties, LogFileName);
        properties->MaximumFileSize = 0; // unbounded
        wcsncpy_s(properties.LogFileName, etlFilePath.c_str(), _TRUNCATE);

        ULONG status = ::StartTraceW(&g_sessionHandle, g_sessionName.c_str(), properties);
        if (status != ERROR_SUCCESS)
        {
            printf("StartTraceW (record) failed: %lu (are you in the 'Performance Log Users' group?)\n", status);
            return false;
        }

        EnableD3D12Provider(g_sessionHandle, true);
        EnableDxgkProvider(g_sessionHandle, true, true);
        return true;
    }

    // Launches a target suspended so we can start the trace before it creates any
    // D3D12 objects - creation events (and explicit heaps / placed resources,
    // which D3D12's rundown does not re-emit) only show up if we're listening
    // from the start. Returns the pid and the suspended main-thread handle for
    // the caller to resume once capture is live.
    bool LaunchSuspended(const std::wstring& commandLine, DWORD& processId, HANDLE& mainThread)
    {
        std::wstring mutableCommandLine = commandLine;
        STARTUPINFOW startupInfo{ sizeof(startupInfo) };
        PROCESS_INFORMATION processInfo{};

        if (!CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED, nullptr, nullptr, &startupInfo, &processInfo))
        {
            printf("CreateProcess failed for '%ls': %lu\n", commandLine.c_str(), GetLastError());
            return false;
        }

        CloseHandle(processInfo.hProcess);
        processId = processInfo.dwProcessId;
        mainThread = processInfo.hThread;
        return true;
    }

    // Offline replay of a saved .etl. Runs to completion synchronously.
    bool ReplayEtlFile(const std::wstring& etlFilePath, DWORD processId)
    {
        CreateHandler(processId);

        EVENT_TRACE_LOGFILE logFile{};
        logFile.LogFileName = const_cast<LPWSTR>(etlFilePath.c_str());
        logFile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        logFile.EventRecordCallback = OnEvent;
        logFile.BufferCallback = OnBuffer;

        TRACEHANDLE traceHandle = OpenTrace(&logFile);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            printf("OpenTrace failed for '%ls': %lu\n", etlFilePath.c_str(), GetLastError());
            return false;
        }

        ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        g_handler->OnDataComplete();
        CloseTrace(traceHandle);

        if (status != ERROR_SUCCESS)
        {
            printf("ProcessTrace failed for '%ls': %lu\n", etlFilePath.c_str(), status);
            return false;
        }
        return true;
    }

    // ---- Formatting ---------------------------------------------------------

    std::wstring FormatBytes(uint64_t bytes)
    {
        const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
        double value = static_cast<double>(bytes);
        int unit = 0;
        while (value >= 1024.0 && unit < 4)
        {
            value /= 1024.0;
            ++unit;
        }
        wchar_t buffer[64];
        swprintf(buffer, 64, (unit == 0) ? L"%.0f %s" : L"%.1f %s", value, units[unit]);
        return buffer;
    }

    const wchar_t* KindLabel(ObjectKind kind)
    {
        switch (kind)
        {
        case ObjectKind::Heap:              return L"Heap";
        case ObjectKind::CommittedResource: return L"Committed";
        case ObjectKind::PlacedResource:    return L"Placed";
        case ObjectKind::ReservedResource:  return L"Reserved";
        }
        return L"?";
    }

    std::wstring DisplayName(const ObjectRecord& object)
    {
        if (!object.Name.empty())
        {
            return object.Name;
        }
        wchar_t buffer[32];
        swprintf(buffer, 32, L"(id %llu)", static_cast<unsigned long long>(object.Id));
        return buffer;
    }

    // ImGui speaks UTF-8; our model and the ETW event names are wide.
    std::string ToUtf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
        return result;
    }

    // ---- Headless text dump -------------------------------------------------

    void PrintPanel(const wchar_t* title, const Panel& panel)
    {
        wprintf(L"%s  (total %s, %zu cells)\n", title, FormatBytes(panel.TotalBytes).c_str(), panel.Cells.size());
        uint64_t currentHeapBase = 0;
        for (const auto& cell : panel.Cells)
        {
            if (cell.Object == nullptr)
            {
                continue;
            }
            if (!cell.IsChild)
            {
                currentHeapBase = cell.Object->BaseAddress;
            }
            wchar_t offsetText[64] = L"";
            if (cell.IsChild && cell.Object->BaseAddress >= currentHeapBase)
            {
                swprintf(offsetText, 64, L" +%s into heap", FormatBytes(cell.Object->BaseAddress - currentHeapBase).c_str());
            }
            wprintf(L"  %s[%-9s] %-32s %-9s @ 0x%llX%ls\n",
                cell.IsChild ? L"    " : L"",
                KindLabel(cell.Object->Kind),
                DisplayName(*cell.Object).c_str(),
                FormatBytes(cell.Object->Size).c_str(),
                cell.Object->BaseAddress,
                offsetText);
        }
    }

    void DumpModel()
    {
        auto snapshot = g_model.Snapshot();

        // Geometry is irrelevant for text; feed a unit rect so nesting still runs.
        treemap::Rect dummy{ 0, 0, 1000, 1000 };
        Scene scene = BuildScene(snapshot, dummy, dummy);

        wprintf(L"\n==== Memory map (%zu tracked objects) ====\n\n", snapshot.size());
        PrintPanel(L"SYSTEM memory (host RAM, NonLocal)", scene.System);
        wprintf(L"\n");
        PrintPanel(L"VIDEO memory (VRAM, Local)", scene.Video);

        if (!scene.Unresolved.empty())
        {
            wprintf(L"\nUnresolved - segment group not seen yet (%zu):\n", scene.Unresolved.size());
            for (const auto* object : scene.Unresolved)
            {
                wprintf(L"  [%-9s] %-32s %s\n",
                    KindLabel(object->Kind),
                    DisplayName(*object).c_str(),
                    FormatBytes(object->Size).c_str());
            }
        }
        wprintf(L"\n");
    }

    // ---- ImGui / DX11 host --------------------------------------------------

    constexpr float HeaderHeight = 30.0f;
    constexpr float FooterHeight = 24.0f;
    constexpr float Margin = 10.0f;
    constexpr float Gutter = 12.0f;

    std::wstring g_sourceDescription;

    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_deviceContext = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;
    ID3D11RenderTargetView* g_renderTargetView = nullptr;

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (backBuffer != nullptr)
        {
            g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTargetView);
            backBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (g_renderTargetView != nullptr)
        {
            g_renderTargetView->Release();
            g_renderTargetView = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL obtained{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &desc, &g_swapChain, &g_device, &obtained, &g_deviceContext);
        if (FAILED(hr))
        {
            return false;
        }

        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D()
    {
        CleanupRenderTarget();
        if (g_swapChain != nullptr) { g_swapChain->Release(); g_swapChain = nullptr; }
        if (g_deviceContext != nullptr) { g_deviceContext->Release(); g_deviceContext = nullptr; }
        if (g_device != nullptr) { g_device->Release(); g_device = nullptr; }
    }

    // ---- Treemap drawing via ImGui's draw list ------------------------------

    ImU32 KindColor(ObjectKind kind)
    {
        switch (kind)
        {
        case ObjectKind::Heap:              return IM_COL32(70, 110, 160, 255);
        case ObjectKind::CommittedResource: return IM_COL32(80, 150, 90, 255);
        case ObjectKind::PlacedResource:    return IM_COL32(210, 140, 60, 255);
        case ObjectKind::ReservedResource:  return IM_COL32(150, 95, 165, 255);
        }
        return IM_COL32(120, 120, 120, 255);
    }

    ImU32 CellColor(const Cell& cell)
    {
        if (cell.Object == nullptr)
        {
            return IM_COL32(60, 60, 64, 255); // free space inside a heap
        }
        return KindColor(cell.Object->Kind);
    }

    void DrawCell(ImDrawList* drawList, const Cell& cell)
    {
        if (cell.Rect.Width < 1.0f || cell.Rect.Height < 1.0f)
        {
            return;
        }

        ImVec2 topLeft(cell.Rect.X, cell.Rect.Y);
        ImVec2 bottomRight(cell.Rect.X + cell.Rect.Width, cell.Rect.Y + cell.Rect.Height);

        drawList->AddRectFilled(topLeft, bottomRight, CellColor(cell));
        drawList->AddRect(topLeft, bottomRight, IM_COL32(25, 25, 28, 255));

        // Text is clipped to the cell below, so we only need enough room to make
        // a glyph or two worthwhile. Don't gate on width - a tall, narrow cell
        // (common for the odd one out in a squarified strip) should still show
        // its name rather than sitting there as a mystery blank square.
        if (cell.Object == nullptr || cell.Rect.Width < 12.0f || cell.Rect.Height < 14.0f)
        {
            return;
        }

        // Clip labels to the cell so long names don't spill into neighbours.
        drawList->PushClipRect(topLeft, bottomRight, true);
        drawList->AddText(ImVec2(topLeft.x + 3, topLeft.y + 1), IM_COL32(255, 255, 255, 255),
            ToUtf8(DisplayName(*cell.Object)).c_str());
        if (cell.Rect.Height >= 30.0f)
        {
            drawList->AddText(ImVec2(topLeft.x + 3, topLeft.y + 16), IM_COL32(220, 220, 220, 255),
                ToUtf8(FormatBytes(cell.Object->Size)).c_str());
        }
        drawList->PopClipRect();
    }

    void DrawPanel(ImDrawList* drawList, const treemap::Rect& bounds, const char* title, const Panel& panel)
    {
        ImVec2 topLeft(bounds.X, bounds.Y);
        ImVec2 bottomRight(bounds.X + bounds.Width, bounds.Y + bounds.Height);

        drawList->AddRectFilled(topLeft, bottomRight, IM_COL32(40, 40, 44, 255));
        for (const auto& cell : panel.Cells)
        {
            DrawCell(drawList, cell);
        }
        drawList->AddRect(topLeft, bottomRight, IM_COL32(90, 90, 96, 255));

        std::string heading = std::string(title) + "  -  " + ToUtf8(FormatBytes(panel.TotalBytes));
        drawList->AddText(ImVec2(bounds.X, bounds.Y - HeaderHeight + 6), IM_COL32(230, 230, 230, 255), heading.c_str());
    }

    const char* GroupLabel(MemorySegmentGroup group)
    {
        switch (group)
        {
        case MemorySegmentGroup::Local:    return "video (local)";
        case MemorySegmentGroup::NonLocal: return "system (non-local)";
        default:                           return "unknown";
        }
    }

    // Finds the top-most cell under a point. Cells are stored parent-before-child,
    // so scanning back to front lets a placed resource win over its heap.
    const ObjectRecord* HitTest(const Panel& panel, ImVec2 point)
    {
        for (auto it = panel.Cells.rbegin(); it != panel.Cells.rend(); ++it)
        {
            const Cell& cell = *it;
            if (cell.Object == nullptr)
            {
                continue;
            }
            if (point.x >= cell.Rect.X && point.x < cell.Rect.X + cell.Rect.Width &&
                point.y >= cell.Rect.Y && point.y < cell.Rect.Y + cell.Rect.Height)
            {
                return cell.Object;
            }
        }
        return nullptr;
    }

    void ShowTooltip(const ObjectRecord& object, const std::vector<ObjectRecord>& snapshot)
    {
        ImGui::BeginTooltip();
        ImGui::Text("%s", ToUtf8(DisplayName(object)).c_str());
        ImGui::Separator();
        ImGui::Text("Type:    %ls", KindLabel(object.Kind));
        ImGui::Text("Size:    %ls", FormatBytes(object.Size).c_str());
        ImGui::Text("Address: 0x%llX", static_cast<unsigned long long>(object.BaseAddress));
        ImGui::Text("Memory:  %s", GroupLabel(object.Group));

        // For a placed resource, spell out where it actually lives: its offset in
        // the backing heap, and how many other resources share those same bytes.
        // Transient allocators (e.g. UE5) alias many resources at one offset, so
        // the lane stacking alone doesn't make the position obvious.
        if (object.Kind == ObjectKind::PlacedResource)
        {
            const ObjectRecord* parentHeap = nullptr;
            for (const auto& candidate : snapshot)
            {
                if (candidate.Kind == ObjectKind::Heap && HeapContains(candidate, object.BaseAddress))
                {
                    parentHeap = &candidate;
                    break;
                }
            }

            if (parentHeap != nullptr)
            {
                const uint64_t offset = object.BaseAddress - parentHeap->BaseAddress;
                ImGui::Separator();
                ImGui::Text("In heap: %s", ToUtf8(DisplayName(*parentHeap)).c_str());
                ImGui::Text("Offset:  +%ls into heap", FormatBytes(offset).c_str());

                const uint64_t start = object.BaseAddress;
                const uint64_t end = object.BaseAddress + object.Size;
                int aliasCount = 0;
                for (const auto& other : snapshot)
                {
                    if (&other == &object || other.Kind != ObjectKind::PlacedResource)
                    {
                        continue;
                    }
                    if (!HeapContains(*parentHeap, other.BaseAddress))
                    {
                        continue;
                    }
                    if (start < other.BaseAddress + other.Size && other.BaseAddress < end)
                    {
                        ++aliasCount;
                    }
                }
                if (aliasCount > 0)
                {
                    ImGui::Text("Aliases: %d other resource%s sharing these bytes",
                        aliasCount, aliasCount == 1 ? "" : "s");
                }
            }
        }
        ImGui::EndTooltip();
    }

    // Lays the current model into the two panels and paints them into a
    // borderless window that fills the viewport.
    void DrawScene()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
        ImGui::Begin("MemoryMap", nullptr, flags);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();

        drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(28, 28, 30, 255));

        float panelTop = origin.y + HeaderHeight;
        float panelHeight = size.y - HeaderHeight - FooterHeight;
        float halfWidth = (size.x - 2 * Margin - Gutter) / 2.0f;

        if (panelHeight >= 10.0f && halfWidth >= 10.0f)
        {
            treemap::Rect systemRect{ origin.x + Margin, panelTop, halfWidth, panelHeight };
            treemap::Rect videoRect{ origin.x + Margin + halfWidth + Gutter, panelTop, halfWidth, panelHeight };

            auto snapshot = g_model.Snapshot();
            Scene scene = BuildScene(snapshot, systemRect, videoRect);

            DrawPanel(drawList, systemRect, "System memory (host RAM)", scene.System);
            DrawPanel(drawList, videoRect, "Video memory (VRAM)", scene.Video);

            ImVec2 mouse = ImGui::GetMousePos();
            const ObjectRecord* hovered = HitTest(scene.System, mouse);
            if (hovered == nullptr)
            {
                hovered = HitTest(scene.Video, mouse);
            }
            if (hovered != nullptr)
            {
                ShowTooltip(*hovered, snapshot);
            }

            const float footerY = origin.y + size.y - FooterHeight + 4;
            const ImU32 footerColor = IM_COL32(200, 200, 200, 255);
            float footerX = origin.x + Margin;

            auto drawFooterText = [&](const char* text, ImU32 color)
            {
                drawList->AddText(ImVec2(footerX, footerY), color, text);
                footerX += ImGui::CalcTextSize(text).x;
            };

            char leadIn[192];
            snprintf(leadIn, sizeof(leadIn), "%s   |   objects: %zu   |   ",
                ToUtf8(g_sourceDescription).c_str(), snapshot.size());
            drawFooterText(leadIn, footerColor);

            // Remember where "unresolved: N" sits so we can list the names on hover.
            char unresolvedText[48];
            snprintf(unresolvedText, sizeof(unresolvedText), "unresolved: %zu", scene.Unresolved.size());
            const float unresolvedLeft = footerX;
            const bool unresolvedInteractive = !scene.Unresolved.empty();
            drawFooterText(unresolvedText, unresolvedInteractive ? IM_COL32(230, 190, 90, 255) : footerColor);
            const float unresolvedRight = footerX;

            drawFooterText("   |   ", footerColor);

            // Legend, but only for the object kinds actually on screen - so
            // "reserved" shows up only when something tiled is present.
            bool kindPresent[4] = {};
            for (const auto& cell : scene.System.Cells)
            {
                if (cell.Object != nullptr) { kindPresent[static_cast<int>(cell.Object->Kind)] = true; }
            }
            for (const auto& cell : scene.Video.Cells)
            {
                if (cell.Object != nullptr) { kindPresent[static_cast<int>(cell.Object->Kind)] = true; }
            }

            struct LegendEntry { ObjectKind Kind; const char* Label; };
            const LegendEntry legend[] = {
                { ObjectKind::Heap, "heap" },
                { ObjectKind::CommittedResource, "committed" },
                { ObjectKind::PlacedResource, "placed" },
                { ObjectKind::ReservedResource, "reserved" },
            };
            for (const auto& entry : legend)
            {
                if (!kindPresent[static_cast<int>(entry.Kind)])
                {
                    continue;
                }
                const ImVec2 swatchMin(footerX, footerY + 1);
                const ImVec2 swatchMax(footerX + 11, footerY + 12);
                drawList->AddRectFilled(swatchMin, swatchMax, KindColor(entry.Kind));
                drawList->AddRect(swatchMin, swatchMax, IM_COL32(25, 25, 28, 255));
                footerX += 15;
                drawFooterText(entry.Label, footerColor);
                footerX += 12;
            }

            // Hovering the unresolved count lists which objects are still waiting
            // for a segment-group event (so you can tell what hasn't landed yet).
            if (hovered == nullptr && unresolvedInteractive &&
                mouse.x >= unresolvedLeft && mouse.x < unresolvedRight &&
                mouse.y >= footerY && mouse.y < footerY + FooterHeight)
            {
                ImGui::BeginTooltip();
                ImGui::Text("Waiting for a video/system memory event (%zu):", scene.Unresolved.size());
                ImGui::Separator();
                size_t shown = 0;
                for (const auto* object : scene.Unresolved)
                {
                    if (shown++ >= 30)
                    {
                        ImGui::Text("... and %zu more", scene.Unresolved.size() - 30);
                        break;
                    }
                    ImGui::Text("%s  (%ls)", ToUtf8(DisplayName(*object)).c_str(), KindLabel(object->Kind));
                }
                ImGui::EndTooltip();
            }
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
        {
            return 1;
        }

        switch (message)
        {
        case WM_SIZE:
            if (g_device != nullptr && wParam != SIZE_MINIMIZED)
            {
                CleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    int RunWindow()
    {
        HINSTANCE instance = GetModuleHandle(nullptr);

        WNDCLASSEXW windowClass{ sizeof(windowClass) };
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"DxTimingCaptureMemoryMapWindow";
        RegisterClassExW(&windowClass);

        HWND hwnd = CreateWindowExW(
            0, windowClass.lpszClassName, L"DxTimingCaptureLibrary - Heap & Resource Map (system | video)",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 1100, 640,
            nullptr, nullptr, instance, nullptr);
        if (hwnd == nullptr)
        {
            return 1;
        }

        if (!CreateDeviceD3D(hwnd))
        {
            CleanupDeviceD3D();
            UnregisterClassW(windowClass.lpszClassName, instance);
            printf("D3D11 device creation failed.\n");
            return 1;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr; // don't drop an imgui.ini next to the exe
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX11_Init(g_device, g_deviceContext);

        bool done = false;
        while (!done)
        {
            MSG message;
            while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessage(&message);
                if (message.message == WM_QUIT)
                {
                    done = true;
                }
            }
            if (done)
            {
                break;
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            DrawScene();

            ImGui::Render();
            g_deviceContext->OMSetRenderTargets(1, &g_renderTargetView, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_swapChain->Present(1, 0); // vsync
        }

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, instance);
        return 0;
    }

    // ---- Command line -------------------------------------------------------

    void PrintUsage()
    {
        wprintf(L"\n"
            L"DxTimingCaptureLibrary memory-map sample.\n"
            L"\n"
            L"Draws two WinDirStat-style treemaps of a process's D3D12 heaps and\n"
            L"resources: system memory (host RAM) on the left, video memory (VRAM)\n"
            L"on the right. Placed resources nest inside their heap.\n"
            L"\n"
            L"Usage:\n"
            L"  DxTimingCaptureLibrary.sample.memorymap --pid <id> [--dump] [--seconds <n>]\n"
            L"  DxTimingCaptureLibrary.sample.memorymap --launch <exe> [--dump] [--seconds <n>]\n"
            L"  DxTimingCaptureLibrary.sample.memorymap --etl <path> --pid <id> [--dump]\n"
            L"\n"
            L"  --pid <id>     Observe an already-running process.\n"
            L"  --launch <exe> Launch <exe> suspended and capture it from the start.\n"
            L"                 Best for seeing explicit heaps and placed resources,\n"
            L"                 which D3D12's rundown does not re-emit.\n"
            L"  --etl <path>   Replay a saved .etl instead of capturing live.\n"
            L"  --demo         Show synthetic data (no GPU or ETW needed).\n"
            L"  --dump         Print the model as text and exit (no window).\n"
            L"  --seconds <n>  Live --dump capture duration (default 10).\n");
    }
}

int __cdecl wmain(int argc, wchar_t** argv)
{
    DWORD processId = 0;
    std::wstring etlFilePath;
    std::wstring launchPath;
    std::wstring recordPath;
    bool dumpMode = false;
    bool demoMode = false;
    int captureSeconds = 10;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        auto takeValue = [&](std::wstring& out) -> bool
        {
            if (i + 1 >= argc)
            {
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (arg == L"--help" || arg == L"-h")
        {
            PrintUsage();
            return 0;
        }
        else if (arg == L"--pid")
        {
            std::wstring value;
            if (!takeValue(value))
            {
                PrintUsage();
                return 1;
            }
            processId = static_cast<DWORD>(wcstoul(value.c_str(), nullptr, 10));
        }
        else if (arg == L"--etl")
        {
            if (!takeValue(etlFilePath))
            {
                PrintUsage();
                return 1;
            }
        }
        else if (arg == L"--record")
        {
            if (!takeValue(recordPath))
            {
                PrintUsage();
                return 1;
            }
        }
        else if (arg == L"--launch")
        {
            if (!takeValue(launchPath))
            {
                PrintUsage();
                return 1;
            }
        }
        else if (arg == L"--dump")
        {
            dumpMode = true;
        }
        else if (arg == L"--demo")
        {
            demoMode = true;
        }
        else if (arg == L"--seconds")
        {
            std::wstring value;
            if (!takeValue(value))
            {
                PrintUsage();
                return 1;
            }
            captureSeconds = static_cast<int>(wcstol(value.c_str(), nullptr, 10));
        }
        else
        {
            wprintf(L"Unknown argument: %ls\n", arg.c_str());
            PrintUsage();
            return 1;
        }
    }

    if (demoMode)
    {
        g_sourceDescription = L"demo (synthetic data)";
        PopulateDemoModel();
        if (dumpMode)
        {
            DumpModel();
            return 0;
        }
        return RunWindow();
    }

    if (processId == 0 && launchPath.empty())
    {
        wprintf(L"Provide --pid, --launch, or --demo.\n");
        PrintUsage();
        return 1;
    }

    // ---- Offline replay ----
    if (!etlFilePath.empty())
    {
        if (processId == 0)
        {
            wprintf(L"--etl needs --pid to know which process to decode.\n");
            return 1;
        }
        g_sourceDescription = L"etl: " + etlFilePath;
        if (!ReplayEtlFile(etlFilePath, processId))
        {
            return 1;
        }
        if (dumpMode)
        {
            DumpModel();
            return 0;
        }
        return RunWindow();
    }

    // ---- Record to .etl (for deterministic replay while debugging) ----
    if (!recordPath.empty())
    {
        HANDLE recordThread = nullptr;
        if (!launchPath.empty())
        {
            if (!LaunchSuspended(launchPath, processId, recordThread))
            {
                return 1;
            }
        }

        if (!StartRecordingSession(recordPath))
        {
            if (recordThread != nullptr)
            {
                ResumeThread(recordThread);
                CloseHandle(recordThread);
            }
            return 1;
        }

        if (recordThread != nullptr)
        {
            ResumeThread(recordThread);
            CloseHandle(recordThread);
        }

        wprintf(L"Recording pid %lu to '%ls' for %d seconds...\n", processId, recordPath.c_str(), captureSeconds);
        std::this_thread::sleep_for(std::chrono::seconds(captureSeconds));
        StopLiveSession();
        wprintf(L"Wrote '%ls'. Replay with: --etl \"%ls\" --pid %lu\n", recordPath.c_str(), recordPath.c_str(), processId);
        return 0;
    }

    // ---- Live capture ----
    // In launch mode we start the target suspended, bring the trace up, then
    // resume it so we catch every creation event.
    HANDLE launchedThread = nullptr;
    if (!launchPath.empty())
    {
        if (!LaunchSuspended(launchPath, processId, launchedThread))
        {
            return 1;
        }
    }

    g_sourceDescription = L"live pid " + std::to_wstring(processId);
    if (!StartLiveSession(processId))
    {
        if (launchedThread != nullptr)
        {
            ResumeThread(launchedThread);
            CloseHandle(launchedThread);
        }
        return 1;
    }

    std::thread consumer(LiveConsumerThread);

    if (launchedThread != nullptr)
    {
        ResumeThread(launchedThread);
        CloseHandle(launchedThread);
    }

    int exitCode = 0;
    if (dumpMode)
    {
        wprintf(L"Capturing pid %lu for %d seconds...\n", processId, captureSeconds);
        std::this_thread::sleep_for(std::chrono::seconds(captureSeconds));
        StopLiveSession();
        consumer.join();
        DumpModel();
    }
    else
    {
        // The consumer keeps filling the model while the window pumps messages.
        exitCode = RunWindow();
        StopLiveSession();
        consumer.join();
    }

    return exitCode;
}
