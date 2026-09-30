// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

namespace DirectX::Etw
{

//
// A strongly-typed identifier template that wraps an integral value.
// Uses a Tag type to create distinct types for different identifier purposes.
//
template <typename T, typename Tag>
struct Identifier
{
    static_assert(std::is_integral_v<T>, "Integral type expected");

    T Value{};

    Identifier() = default;

    explicit constexpr Identifier(T value) : Value(value) {}

    explicit operator T() const { return Value; }
    explicit operator bool() const { return Value != T{}; }

    bool operator==(Identifier other) const { return Value == other.Value; }
    bool operator!=(Identifier other) const { return Value != other.Value; }
    bool operator< (Identifier other) const { return Value <  other.Value; }
    bool operator> (Identifier other) const { return Value >  other.Value; }
    bool operator<=(Identifier other) const { return Value <= other.Value; }
    bool operator>=(Identifier other) const { return Value >= other.Value; }
};

} // namespace DirectX::Etw

namespace std
{
    template<typename T, typename Tag>
    struct hash<DirectX::Etw::Identifier<T, Tag>>
    {
        size_t operator()(DirectX::Etw::Identifier<T, Tag> const& id) const
        {
            return std::hash<T>{}(id.Value);
        }
    };
}

namespace DirectX::Etw
{

//
// The PixCounterId is the unique ID for counters reported via the PIXReportCounter
// or built-in system counters reported by the engine.
//
using PixCounterId = Identifier<uint64_t, struct PixCounterId_Tag>;

//
// The PixEventBlockId is the unique ID for decoded PIX event blocks identified
// by a process id and a thread id.
// 64bit id = ((uint64_t)processid) << 32 | threadid
//
using PixEventBlockId = Identifier<uint64_t, struct PixBlockId_Tag>;

//
// WinPixEventContextId is the unique ID for an object identified
// by the WinPixEventRuntime as being associated with a PixEvent.
// The context represents a command list or command queue.
//
using WinPixEventContextId = Identifier<uint64_t, struct WinPixEventContextId_Tag>;

//
// WinPixEventId is the unique ID for an event emitted by the WinPixEventRuntime
//
using WinPixEventId = Identifier<uint64_t, struct WinPixEventId_Tag>;

//
// ApiMarkerEventId is the unique ID for an event written
// to the database for each operation configured on a command list.
//
using ApiMarkerEventId = Identifier<uint64_t, struct ApiMarkerEventIdTag>;

using ApiCommandQueueId = Identifier<uint64_t, struct ApiCommandQueueId_Tag>;
using HardwareCommandQueueId = Identifier<uint64_t, struct HardwareCommandQueueId_Tag>;
using AdapterId = Identifier<uint64_t, struct AdapterId_Tag>;
using MonitorId = Identifier<uint64_t, struct MonitorId_Tag>;

// Monitors are unique by their vidPnTargetId and dxgkAdapterId, since target IDs are per-adapter
using MonitorIdentifier = std::pair<uint32_t, uint64_t>;

} // namespace DirectX::Etw