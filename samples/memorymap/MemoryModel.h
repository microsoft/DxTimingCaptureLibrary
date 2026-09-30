// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <DxTimingCaptureLibrary/Types.h>

#include "Treemap.h"

namespace memorymap
{
    using DirectX::Etw::MemorySegmentGroup;

    enum class ObjectKind
    {
        Heap,
        CommittedResource,
        PlacedResource,
        ReservedResource,
    };

    // One tracked heap or resource. Ids are the ones we hand back from the
    // creation callbacks, so every later callback (name, size, migration,
    // destruction) refers to the object by the same id.
    struct ObjectRecord
    {
        uint64_t Id = 0;
        ObjectKind Kind = ObjectKind::Heap;
        std::wstring Name;
        uint64_t Size = 0;
        uint64_t BaseAddress = 0;         // GPU virtual address
        MemorySegmentGroup Group = MemorySegmentGroup::Unknown;
    };

    // Thread-safe store. The ETW consumer thread writes; the UI thread snapshots.
    class MemoryModel
    {
    public:
        void Add(const ObjectRecord& record)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_objects[record.Id] = record;
        }

        void SetName(uint64_t id, std::wstring_view name)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (auto it = m_objects.find(id); it != m_objects.end())
            {
                it->second.Name.assign(name);
            }
        }

        void SetSizeAndAddress(uint64_t id, uint64_t size, uint64_t baseAddress)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (auto it = m_objects.find(id); it != m_objects.end())
            {
                if (size != 0)
                {
                    it->second.Size = size;
                }
                if (baseAddress != 0)
                {
                    it->second.BaseAddress = baseAddress;
                }
            }
        }

        void SetGroup(uint64_t id, MemorySegmentGroup group)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (auto it = m_objects.find(id); it != m_objects.end())
            {
                it->second.Group = group;
            }
        }

        void Remove(uint64_t id)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_objects.erase(id);
        }

        std::vector<ObjectRecord> Snapshot() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::vector<ObjectRecord> result;
            result.reserve(m_objects.size());
            for (const auto& [id, record] : m_objects)
            {
                result.push_back(record);
            }
            return result;
        }

    private:
        mutable std::mutex m_mutex;
        std::unordered_map<uint64_t, ObjectRecord> m_objects;
    };

    // ---- Scene: the objects laid out into the two panels ---------------------

    // A single drawable rectangle. Object is null for the free space left over
    // inside a heap.
    struct Cell
    {
        const ObjectRecord* Object = nullptr;
        treemap::Rect Rect;
        bool IsChild = false; // a placed resource drawn inside its heap
    };

    struct Panel
    {
        std::vector<Cell> Cells;
        uint64_t TotalBytes = 0;
    };

    struct Scene
    {
        Panel System; // NonLocal - host RAM
        Panel Video;  // Local - VRAM
        std::vector<const ObjectRecord*> Unresolved; // group not known yet
    };

    // Height reserved at the top of a heap cell for its own label, so nested
    // placed resources don't cover it.
    constexpr float HeapTitleBarHeight = 16.0f;

    inline bool GroupToPanelIsVideo(MemorySegmentGroup group, bool& isVideo)
    {
        switch (group)
        {
        case MemorySegmentGroup::Local:    isVideo = true;  return true;
        case MemorySegmentGroup::NonLocal: isVideo = false; return true;
        default:                           return false; // Unknown
        }
    }

    inline bool HeapContains(const ObjectRecord& heap, uint64_t address)
    {
        return heap.Size != 0
            && address >= heap.BaseAddress
            && address < heap.BaseAddress + heap.Size;
    }

    // Turns a snapshot into two laid-out panels. Placed resources are nested
    // inside the heap whose address range contains them, so a heap and its
    // contents are never double-counted and a heap migrating between pools
    // carries all its placed resources with it.
    inline Scene BuildScene(const std::vector<ObjectRecord>& objects, treemap::Rect systemRect, treemap::Rect videoRect)
    {
        Scene scene;

        // Index heaps so placed resources can find their parent by address.
        std::vector<const ObjectRecord*> heaps;
        for (const auto& object : objects)
        {
            if (object.Kind == ObjectKind::Heap)
            {
                heaps.push_back(&object);
            }
        }

        auto findParentHeap = [&](const ObjectRecord& placed) -> const ObjectRecord*
        {
            for (const auto* heap : heaps)
            {
                if (HeapContains(*heap, placed.BaseAddress))
                {
                    return heap;
                }
            }
            return nullptr;
        };

        // Group placed resources by their parent heap id.
        std::unordered_map<uint64_t, std::vector<const ObjectRecord*>> placedByHeap;
        std::vector<const ObjectRecord*> orphanPlaced; // no known parent heap

        for (const auto& object : objects)
        {
            if (object.Kind != ObjectKind::PlacedResource)
            {
                continue;
            }
            if (const auto* parent = findParentHeap(object))
            {
                placedByHeap[parent->Id].push_back(&object);
            }
            else
            {
                orphanPlaced.push_back(&object);
            }
        }

        // Collect the top-level nodes for each panel. A heap inherits the whole
        // panel from its own group; placed resources follow their heap.
        struct TopNode
        {
            const ObjectRecord* Object;
            uint64_t Weight;
        };
        std::vector<TopNode> systemNodes;
        std::vector<TopNode> videoNodes;

        auto placeTopLevel = [&](const ObjectRecord& object, uint64_t weight)
        {
            bool isVideo = false;
            if (!GroupToPanelIsVideo(object.Group, isVideo))
            {
                scene.Unresolved.push_back(&object);
                return;
            }
            (isVideo ? videoNodes : systemNodes).push_back({ &object, weight });
        };

        for (const auto& object : objects)
        {
            switch (object.Kind)
            {
            case ObjectKind::Heap:
            case ObjectKind::CommittedResource:
            case ObjectKind::ReservedResource:
                placeTopLevel(object, object.Size);
                break;
            case ObjectKind::PlacedResource:
                // Only orphans become their own top-level node; the rest are
                // drawn nested below.
                break;
            }
        }
        for (const auto* placed : orphanPlaced)
        {
            placeTopLevel(*placed, placed->Size);
        }

        auto buildPanel = [&](std::vector<TopNode>& nodes, treemap::Rect bounds) -> Panel
        {
            Panel panel;

            std::vector<double> weights;
            weights.reserve(nodes.size());
            for (const auto& node : nodes)
            {
                weights.push_back(static_cast<double>(node.Weight));
                panel.TotalBytes += node.Weight;
            }

            std::vector<treemap::Rect> rects;
            treemap::Squarify(weights, bounds, rects);

            for (size_t i = 0; i < nodes.size(); ++i)
            {
                const ObjectRecord* object = nodes[i].Object;
                const treemap::Rect rect = rects[i];

                panel.Cells.push_back(Cell{ object, rect, false });

                // Nest placed resources inside a heap's body.
                if (object->Kind != ObjectKind::Heap)
                {
                    continue;
                }
                auto childrenIt = placedByHeap.find(object->Id);
                if (childrenIt == placedByHeap.end() || rect.Height <= HeapTitleBarHeight + 2 || rect.Width <= 4)
                {
                    continue;
                }

                treemap::Rect body{ rect.X + 1, rect.Y + HeapTitleBarHeight, rect.Width - 2, rect.Height - HeapTitleBarHeight - 1 };

                // Lay each placed resource at its real offset within the heap's
                // address range. Resources that alias the same bytes overlap in
                // address, so we stack them in separate lanes to make the overlap
                // visible; any address the resources don't cover is left as the
                // heap's own fill, which reads as free space.
                const auto& children = childrenIt->second;
                const double heapSize = static_cast<double>(object->Size);

                struct Span
                {
                    const ObjectRecord* Object;
                    float Left;
                    float Right;
                };
                std::vector<Span> spans;
                spans.reserve(children.size());
                for (const auto* child : children)
                {
                    const uint64_t offset = (child->BaseAddress > object->BaseAddress)
                        ? (child->BaseAddress - object->BaseAddress)
                        : 0;
                    double startFraction = static_cast<double>(offset) / heapSize;
                    double endFraction = static_cast<double>(offset + child->Size) / heapSize;
                    startFraction = std::min(startFraction, 1.0);
                    endFraction = std::min(endFraction, 1.0);

                    float left = body.X + static_cast<float>(startFraction * body.Width);
                    float right = body.X + static_cast<float>(endFraction * body.Width);
                    if (right < left + 2.0f)
                    {
                        right = left + 2.0f; // keep sub-pixel resources hoverable
                    }
                    spans.push_back({ child, left, right });
                }

                std::ranges::sort(spans, {}, &Span::Left);

                // Greedy interval coloring: a lane holds resources whose address
                // ranges don't overlap, so aliased resources land in fresh lanes.
                std::vector<float> laneRightEdge;
                std::vector<int> laneOfSpan(spans.size());
                for (size_t s = 0; s < spans.size(); ++s)
                {
                    int lane = -1;
                    for (size_t l = 0; l < laneRightEdge.size(); ++l)
                    {
                        if (spans[s].Left >= laneRightEdge[l])
                        {
                            lane = static_cast<int>(l);
                            break;
                        }
                    }
                    if (lane < 0)
                    {
                        lane = static_cast<int>(laneRightEdge.size());
                        laneRightEdge.push_back(0.0f);
                    }
                    laneRightEdge[lane] = spans[s].Right;
                    laneOfSpan[s] = lane;
                }

                const size_t laneCount = laneRightEdge.empty() ? 1 : laneRightEdge.size();
                const float laneHeight = body.Height / static_cast<float>(laneCount);

                for (size_t s = 0; s < spans.size(); ++s)
                {
                    const float top = body.Y + laneOfSpan[s] * laneHeight;
                    const treemap::Rect childRect{ spans[s].Left, top, spans[s].Right - spans[s].Left, laneHeight };
                    panel.Cells.push_back(Cell{ spans[s].Object, childRect, true });
                }
            }

            return panel;
        };

        scene.System = buildPanel(systemNodes, systemRect);
        scene.Video = buildPanel(videoNodes, videoRect);
        return scene;
    }
}
