// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "TicksToNanoseconds.h"
#include <optional>
#include <iterator>

#include <windows.h>
#include <evntrace.h>

#include <DxTimingCaptureLibrary/EtwException.h>

// Helper classes to help interpret raw ETW data
namespace DirectX::Etw
{
    class PointerIterator
    {
        uint8_t* const m_dataStart;
        uint32_t m_dataLength;
        int const m_pointerByteSize;
        uint8_t* const m_dataEnd;
        uint8_t* m_remainingData;

        std::optional<uint64_t> m_currentValue;

    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = uint64_t;
        using difference_type = std::ptrdiff_t;
        using pointer = uint64_t const*;
        using reference = uint64_t const&;

        explicit PointerIterator(uint8_t* const data, uint32_t datalength, int pointerSize, bool endIterator = false)
            : m_dataStart(data)
            , m_dataLength(datalength)
            , m_pointerByteSize(pointerSize)
            , m_dataEnd(m_dataStart + m_dataLength)
            , m_remainingData(endIterator ? m_dataEnd : m_dataStart)
        {
            increment();
        }

        reference operator*() const { return *m_currentValue; }
        pointer operator->() const { return &(*m_currentValue); }

        PointerIterator& operator++()
        {
            increment();
            return *this;
        }

        PointerIterator operator++(int)
        {
            PointerIterator tmp = *this;
            increment();
            return tmp;
        }

        bool operator==(PointerIterator const& other) const
        {
            return (this->m_dataStart == other.m_dataStart) &&
                   (this->m_dataLength == other.m_dataLength) &&
                   (this->m_remainingData == other.m_remainingData) &&
                   (this->m_currentValue == other.m_currentValue);
        }

        bool operator!=(PointerIterator const& other) const
        {
            return !(*this == other);
        }

    private:
        void increment() { m_currentValue = TryReadPointerValue(); }

        std::optional<uint64_t> TryReadPointerValue()
        {
            switch (m_pointerByteSize)
            {
            case 4:
                return TryReadValue<uint32_t>();
            case 8:
                return TryReadValue<uint64_t>();
            default:
                return std::nullopt;
            }
        }

        template<typename T>
        std::optional<uint64_t> TryReadValue()
        {
            auto* newPos = m_remainingData + sizeof(T);
            if (newPos > m_dataEnd)
                return std::nullopt;

            auto* d = m_remainingData;
            m_remainingData = newPos;
            return *reinterpret_cast<T*>(d);
        }
    };

    class PointerRange
    {
        PointerIterator m_begin;
        PointerIterator m_end;

    public:
        PointerRange(PointerIterator begin, PointerIterator end)
            : m_begin(std::move(begin))
            , m_end(std::move(end))
        {
        }

        PointerIterator begin() const { return m_begin; }
        PointerIterator end() const { return m_end; }
    };

    class EventData
    {
        EVENT_RECORD* const m_record;
        int const m_pointerByteSize;
        uint8_t* m_remainingUserData;
        uint8_t* const m_userDataEnd;

    public:
        explicit EventData(EVENT_RECORD* record)
            : m_record(record)
            , m_pointerByteSize(GetPointerByteSize(record))
            , m_remainingUserData(static_cast<uint8_t*>(record->UserData))
            , m_userDataEnd(m_remainingUserData + record->UserDataLength)
        {
        }

        PointerRange ReadPointerArray(uint32_t count)
        {
            size_t pointerArrayDataSize = static_cast<size_t>(count) * m_pointerByteSize;
            uint8_t* pointerArrayDataStart = m_remainingUserData;
            Read(pointerArrayDataSize);

            uint32_t pointerArrayDataLength = static_cast<uint32_t>(pointerArrayDataSize);
            return PointerRange(PointerIterator(pointerArrayDataStart, pointerArrayDataLength, m_pointerByteSize, false),
                                PointerIterator(pointerArrayDataStart, pointerArrayDataLength, m_pointerByteSize, true));
        }

        uint64_t ReadPointer()
        {
            switch (m_pointerByteSize)
            {
            case 4:
                return ReadUint32();
            case 8:
                return ReadUint64();
            default:
                Errors::ThrowToolException(E_UNEXPECTED);
            }
        }

        uint64_t ReadUint64()
        {
            return Read<uint64_t>();
        }

        uint32_t ReadUint32()
        {
            return Read<uint32_t>();
        }

        uint16_t ReadUint16()
        {
            return Read<uint16_t>();
        }

        uint8_t ReadUint8()
        {
            return Read<uint8_t>();
        }

        int8_t ReadInt8()
        {
            return Read<int8_t>();
        }

        int32_t ReadInt32()
        {
            return Read<int32_t>();
        }

        int64_t ReadInt64()
        {
            return Read<int64_t>();
        }

        uint8_t* ReadBytes(uint32_t count)
        {
            return Read(count);
        }

        float ReadFloat()
        {
            return Read<float>();
        }

        GUID ReadGuid()
        {
            return Read<GUID>();
        }

        template<typename T>
        T* ReadArray(uint32_t count)
        {
            return reinterpret_cast<T*>(Read(static_cast<size_t>(count) * sizeof(T)));
        }

        std::wstring_view ReadRemainingAsUnicodeString()
        {
            wchar_t* str = reinterpret_cast<wchar_t*>(m_remainingUserData);
            size_t cchData = ((m_userDataEnd - m_remainingUserData) / sizeof(wchar_t));
            size_t cchLength = wcsnlen(str, cchData);
            m_remainingUserData = m_userDataEnd;
            return std::wstring_view(str, cchLength);
        }

        std::string_view ReadRemainingAsString()
        {
            char* str = reinterpret_cast<char*>(m_remainingUserData);
            size_t cchData = ((m_userDataEnd - m_remainingUserData) / sizeof(char));
            size_t cchString = strnlen(str, cchData);
            m_remainingUserData = m_userDataEnd;
            return std::string_view(str, cchString);
        }

        std::wstring_view ReadSizedWideString()
        {
            uint32_t cch = ReadUint32();
            wchar_t* n = ReadArray<wchar_t>(cch);
            std::wstring_view value(n, cch);
            // The ETW payload's character count includes the null terminator; exclude
            // it so the view holds exactly the string.
            if (!value.empty() && value.back() == L'\0')
            {
                value.remove_suffix(1);
            }
            return value;
        }

        std::string_view ReadSizedString()
        {
            uint32_t cch = ReadUint32();
            char* n = ReadArray<char>(cch);
            std::string_view value(n, cch);
            // The ETW payload's character count includes the null terminator; exclude
            // it so the view holds exactly the string.
            if (!value.empty() && value.back() == '\0')
            {
                value.remove_suffix(1);
            }
            return value;
        }

        std::wstring_view ReadNullTerminatedUnicodeString()
        {
            auto s = std::wstring_view(reinterpret_cast<wchar_t*>(m_remainingUserData));
            m_remainingUserData += ((s.length() + 1) * sizeof(wchar_t));
            return s;
        }

        std::string_view ReadNullTerminatedString()
        {
            auto s = std::string_view(reinterpret_cast<char*>(m_remainingUserData));
            m_remainingUserData += ((s.length() + 1) * sizeof(char));
            return s;
        }

        template<typename T>
        T Load()
        {
            return Read<T>();
        }

        template<>
        std::string_view Load()
        {
            return ReadRemainingAsString();
        }

        template<>
        std::wstring_view Load()
        {
            return ReadRemainingAsUnicodeString();
        }

        template<typename T>
        T LoadNullTerminated()
        {
            return Read<T>();
        }
        
        template<>
        std::string_view LoadNullTerminated()
        {
            return ReadNullTerminatedString();
        }

        template<>
        std::wstring_view LoadNullTerminated()
        {
            return ReadNullTerminatedUnicodeString();
        }

    private:
        template<typename T>
        T Read()
        {
            return *reinterpret_cast<T*>(Read(sizeof(T)));
        }

        uint8_t* Read(size_t numBytes)
        {
            if (numBytes > static_cast<size_t>(m_userDataEnd - m_remainingUserData))
                Errors::ThrowToolException(E_UNEXPECTED);

            auto* d = m_remainingUserData;
            m_remainingUserData += numBytes;
            return d;
        }

        static int GetPointerByteSize(EVENT_RECORD* record)
        {
            if (record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER)
                return 4;
            else if (record->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER)
                return 8;
            else
                Errors::ThrowToolException(E_UNEXPECTED);
        }
    };
} // namespace DirectX::Etw
