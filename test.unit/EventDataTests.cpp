// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <evntcons.h>
#include <vector>

#include <DxTimingCaptureLibrary/EventData.h>

namespace
{
    // Helper to create a mock EVENT_RECORD with the given user data and flags
    struct MockEventRecord
    {
        EVENT_RECORD record = {};
        std::vector<uint8_t> userData;

        MockEventRecord(std::vector<uint8_t> data, USHORT flags)
            : userData(std::move(data))
        {
            record.UserData = userData.data();
            record.UserDataLength = static_cast<USHORT>(userData.size());
            record.EventHeader.Flags = flags;
        }

        EVENT_RECORD* Get() { return &record; }
    };

    std::vector<uint8_t> Make32BitPointerArray(std::initializer_list<uint32_t> values)
    {
        std::vector<uint8_t> result;
        result.reserve(values.size() * sizeof(uint32_t));
        for (uint32_t v : values)
        {
            auto* bytes = reinterpret_cast<uint8_t*>(&v);
            result.insert(result.end(), bytes, bytes + sizeof(uint32_t));
        }
        return result;
    }

    std::vector<uint8_t> Make64BitPointerArray(std::initializer_list<uint64_t> values)
    {
        std::vector<uint8_t> result;
        result.reserve(values.size() * sizeof(uint64_t));
        for (uint64_t v : values)
        {
            auto* bytes = reinterpret_cast<uint8_t*>(&v);
            result.insert(result.end(), bytes, bytes + sizeof(uint64_t));
        }
        return result;
    }
}

// ============================================================================
// ReadPointerArray Tests
// ============================================================================

TEST(EventDataTests, ReadPointerArray_32Bit_SingleElement)
{
    auto data = Make32BitPointerArray({ 0x12345678 });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(1);

    std::vector<uint64_t> result(range.begin(), range.end());
    ASSERT_EQ(1u, result.size());
    EXPECT_EQ(0x12345678u, result[0]);
}

TEST(EventDataTests, ReadPointerArray_32Bit_MultipleElements)
{
    auto data = Make32BitPointerArray({ 0x11111111, 0x22222222, 0x33333333 });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(3);

    std::vector<uint64_t> result(range.begin(), range.end());
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ(0x11111111u, result[0]);
    EXPECT_EQ(0x22222222u, result[1]);
    EXPECT_EQ(0x33333333u, result[2]);
}

TEST(EventDataTests, ReadPointerArray_64Bit_SingleElement)
{
    auto data = Make64BitPointerArray({ 0x123456789ABCDEF0 });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_64_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(1);

    std::vector<uint64_t> result(range.begin(), range.end());
    ASSERT_EQ(1u, result.size());
    EXPECT_EQ(0x123456789ABCDEF0u, result[0]);
}

TEST(EventDataTests, ReadPointerArray_64Bit_MultipleElements)
{
    auto data = Make64BitPointerArray({ 0xAAAAAAAAAAAAAAAA, 0xBBBBBBBBBBBBBBBB, 0xCCCCCCCCCCCCCCCC });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_64_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(3);

    std::vector<uint64_t> result(range.begin(), range.end());
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ(0xAAAAAAAAAAAAAAAAu, result[0]);
    EXPECT_EQ(0xBBBBBBBBBBBBBBBBu, result[1]);
    EXPECT_EQ(0xCCCCCCCCCCCCCCCCu, result[2]);
}

TEST(EventDataTests, ReadPointerArray_32Bit_EmptyArray)
{
    std::vector<uint8_t> data;
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(0);

    std::vector<uint64_t> result(range.begin(), range.end());
    EXPECT_TRUE(result.empty());
}

TEST(EventDataTests, ReadPointerArray_64Bit_EmptyArray)
{
    std::vector<uint8_t> data;
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_64_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(0);

    std::vector<uint64_t> result(range.begin(), range.end());
    EXPECT_TRUE(result.empty());
}

TEST(EventDataTests, ReadPointerArray_32Bit_ZeroValues)
{
    auto data = Make32BitPointerArray({ 0x00000000, 0x00000000 });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(2);

    std::vector<uint64_t> result(range.begin(), range.end());
    ASSERT_EQ(2u, result.size());
    EXPECT_EQ(0u, result[0]);
    EXPECT_EQ(0u, result[1]);
}

TEST(EventDataTests, ReadPointerArray_64Bit_MaxValues)
{
    auto data = Make64BitPointerArray({ UINT64_MAX, UINT64_MAX });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_64_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(2);

    std::vector<uint64_t> result(range.begin(), range.end());
    ASSERT_EQ(2u, result.size());
    EXPECT_EQ(UINT64_MAX, result[0]);
    EXPECT_EQ(UINT64_MAX, result[1]);
}

TEST(EventDataTests, ReadPointerArray_32Bit_MaxValues)
{
    auto data = Make32BitPointerArray({ UINT32_MAX, UINT32_MAX });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(2);

    std::vector<uint64_t> result(range.begin(), range.end());
    ASSERT_EQ(2u, result.size());
    EXPECT_EQ(UINT32_MAX, result[0]);
    EXPECT_EQ(UINT32_MAX, result[1]);
}

TEST(EventDataTests, ReadPointerArray_AdvancesReadPosition)
{
    // Create data with a pointer array followed by a uint32
    auto pointerData = Make32BitPointerArray({ 0x11111111, 0x22222222 });
    uint32_t trailingValue = 0xDEADBEEF;
    pointerData.insert(pointerData.end(),
        reinterpret_cast<uint8_t*>(&trailingValue),
        reinterpret_cast<uint8_t*>(&trailingValue) + sizeof(trailingValue));

    MockEventRecord mockRecord(pointerData, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());

    // Read the pointer array first
    auto range = eventData.ReadPointerArray(2);
    std::vector<uint64_t> result(range.begin(), range.end());
    ASSERT_EQ(2u, result.size());

    // Now read the trailing uint32 - this verifies that ReadPointerArray advanced the position
    uint32_t readTrailingValue = eventData.ReadUint32();
    EXPECT_EQ(0xDEADBEEF, readTrailingValue);
}

// ============================================================================
// PointerIterator Tests
// ============================================================================

TEST(PointerIteratorTests, Iterator_32Bit_CanBeUsedWithRangeBasedFor)
{
    auto data = Make32BitPointerArray({ 0x100, 0x200, 0x300 });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(3);

    uint64_t sum = 0;
    for (uint64_t ptr : range)
    {
        sum += ptr;
    }
    EXPECT_EQ(0x600u, sum);
}

TEST(PointerIteratorTests, Iterator_64Bit_CanBeUsedWithRangeBasedFor)
{
    auto data = Make64BitPointerArray({ 0x1000000000, 0x2000000000, 0x3000000000 });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_64_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(3);

    uint64_t sum = 0;
    for (uint64_t ptr : range)
    {
        sum += ptr;
    }
    EXPECT_EQ(0x6000000000u, sum);
}

TEST(PointerIteratorTests, Iterator_BeginEqualsEndForEmptyRange)
{
    std::vector<uint8_t> data;
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto range = eventData.ReadPointerArray(0);

    EXPECT_EQ(range.begin(), range.end());
}

// ============================================================================
// EventData Basic Read Tests
// ============================================================================

TEST(EventDataTests, ReadUint32_ReturnsCorrectValue)
{
    uint32_t value = 0xCAFEBABE;
    std::vector<uint8_t> data(reinterpret_cast<uint8_t*>(&value),
        reinterpret_cast<uint8_t*>(&value) + sizeof(value));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_EQ(0xCAFEBABE, eventData.ReadUint32());
}

TEST(EventDataTests, ReadUint64_ReturnsCorrectValue)
{
    uint64_t value = 0xDEADBEEFCAFEBABE;
    std::vector<uint8_t> data(reinterpret_cast<uint8_t*>(&value),
        reinterpret_cast<uint8_t*>(&value) + sizeof(value));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_64_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_EQ(0xDEADBEEFCAFEBABE, eventData.ReadUint64());
}

TEST(EventDataTests, ReadUint16_ReturnsCorrectValue)
{
    uint16_t value = 0xABCD;
    std::vector<uint8_t> data(reinterpret_cast<uint8_t*>(&value),
        reinterpret_cast<uint8_t*>(&value) + sizeof(value));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_EQ(0xABCD, eventData.ReadUint16());
}

TEST(EventDataTests, ReadUint8_ReturnsCorrectValue)
{
    std::vector<uint8_t> data = { 0x42 };
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_EQ(0x42, eventData.ReadUint8());
}

TEST(EventDataTests, ReadPointer_32Bit_ReturnsCorrectValue)
{
    auto data = Make32BitPointerArray({ 0x12345678 });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_EQ(0x12345678u, eventData.ReadPointer());
}

TEST(EventDataTests, ReadPointer_64Bit_ReturnsCorrectValue)
{
    auto data = Make64BitPointerArray({ 0x123456789ABCDEF0 });
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_64_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_EQ(0x123456789ABCDEF0u, eventData.ReadPointer());
}

TEST(EventDataTests, ReadInt32_ReturnsCorrectValue)
{
    int32_t value = -12345;
    std::vector<uint8_t> data(reinterpret_cast<uint8_t*>(&value),
        reinterpret_cast<uint8_t*>(&value) + sizeof(value));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_EQ(-12345, eventData.ReadInt32());
}

TEST(EventDataTests, ReadInt64_ReturnsCorrectValue)
{
    int64_t value = -9876543210LL;
    std::vector<uint8_t> data(reinterpret_cast<uint8_t*>(&value),
        reinterpret_cast<uint8_t*>(&value) + sizeof(value));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_64_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_EQ(-9876543210LL, eventData.ReadInt64());
}

TEST(EventDataTests, ReadFloat_ReturnsCorrectValue)
{
    float value = 3.14159f;
    std::vector<uint8_t> data(reinterpret_cast<uint8_t*>(&value),
        reinterpret_cast<uint8_t*>(&value) + sizeof(value));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    EXPECT_FLOAT_EQ(3.14159f, eventData.ReadFloat());
}

TEST(EventDataTests, ReadGuid_ReturnsCorrectValue)
{
    GUID value = { 0x12345678, 0xABCD, 0xEF01, { 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01 } };
    std::vector<uint8_t> data(reinterpret_cast<uint8_t*>(&value),
        reinterpret_cast<uint8_t*>(&value) + sizeof(value));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    GUID result = eventData.ReadGuid();
    EXPECT_EQ(0, memcmp(&value, &result, sizeof(GUID)));
}

TEST(EventDataTests, ReadMultipleValuesInSequence)
{
    // Build buffer with uint32, uint16, uint8
    std::vector<uint8_t> data;
    uint32_t val32 = 0x11223344;
    uint16_t val16 = 0x5566;
    uint8_t val8 = 0x77;

    data.insert(data.end(), reinterpret_cast<uint8_t*>(&val32), reinterpret_cast<uint8_t*>(&val32) + sizeof(val32));
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&val16), reinterpret_cast<uint8_t*>(&val16) + sizeof(val16));
    data.push_back(val8);

    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);
    DirectX::Etw::EventData eventData(mockRecord.Get());

    EXPECT_EQ(0x11223344u, eventData.ReadUint32());
    EXPECT_EQ(0x5566u, eventData.ReadUint16());
    EXPECT_EQ(0x77u, eventData.ReadUint8());
}

TEST(EventDataTests, ReadArray_ReturnsCorrectValues)
{
    std::vector<uint8_t> data = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    uint8_t* result = eventData.ReadArray<uint8_t>(5);

    EXPECT_EQ(0x01, result[0]);
    EXPECT_EQ(0x02, result[1]);
    EXPECT_EQ(0x03, result[2]);
    EXPECT_EQ(0x04, result[3]);
    EXPECT_EQ(0x05, result[4]);
}

// ============================================================================
// ReadNullTerminatedString Tests
// ============================================================================

TEST(EventDataTests, ReadNullTerminatedString_ReturnsCorrectString)
{
    const char* testString = "Hello";
    std::vector<uint8_t> data(testString, testString + strlen(testString) + 1); // Include null terminator
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result = eventData.ReadNullTerminatedString();

    EXPECT_EQ("Hello", result);
    EXPECT_EQ(5u, result.length());
}

TEST(EventDataTests, ReadNullTerminatedString_EmptyString)
{
    std::vector<uint8_t> data = { 0x00 }; // Just null terminator
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result = eventData.ReadNullTerminatedString();

    EXPECT_EQ("", result);
    EXPECT_EQ(0u, result.length());
}

TEST(EventDataTests, ReadNullTerminatedString_MultipleStrings)
{
    std::vector<uint8_t> data;
    const char* str1 = "First";
    const char* str2 = "Second";
    
    data.insert(data.end(), str1, str1 + strlen(str1) + 1);
    data.insert(data.end(), str2, str2 + strlen(str2) + 1);
    
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result1 = eventData.ReadNullTerminatedString();
    auto result2 = eventData.ReadNullTerminatedString();

    EXPECT_EQ("First", result1);
    EXPECT_EQ("Second", result2);
}

TEST(EventDataTests, ReadNullTerminatedString_WithSpecialCharacters)
{
    const char* testString = "Test\t\n123";
    std::vector<uint8_t> data(testString, testString + strlen(testString) + 1);
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result = eventData.ReadNullTerminatedString();

    EXPECT_EQ("Test\t\n123", result);
}

TEST(EventDataTests, ReadNullTerminatedString_AdvancesReadPosition)
{
    std::vector<uint8_t> data;
    const char* str = "TestString";
    uint32_t trailingValue = 0xDEADBEEF;
    
    data.insert(data.end(), str, str + strlen(str) + 1);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&trailingValue),
                reinterpret_cast<uint8_t*>(&trailingValue) + sizeof(trailingValue));
    
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto str_result = eventData.ReadNullTerminatedString();
    auto value = eventData.ReadUint32();

    EXPECT_EQ("TestString", str_result);
    EXPECT_EQ(0xDEADBEEF, value);
}

// ============================================================================
// LoadNullTerminated Tests
// ============================================================================

TEST(EventDataTests, LoadNullTerminated_StringView_ReturnsCorrectString)
{
    const char* testString = "LoadTest";
    std::vector<uint8_t> data(testString, testString + strlen(testString) + 1);
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result = eventData.LoadNullTerminated<std::string_view>();

    EXPECT_EQ("LoadTest", result);
    EXPECT_EQ(8u, result.length());
}

TEST(EventDataTests, LoadNullTerminated_WStringView_ReturnsCorrectString)
{
    const wchar_t* testString = L"WideString";
    std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(testString),
                              reinterpret_cast<const uint8_t*>(testString) + (wcslen(testString) + 1) * sizeof(wchar_t));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result = eventData.LoadNullTerminated<std::wstring_view>();

    EXPECT_EQ(L"WideString", result);
    EXPECT_EQ(10u, result.length());
}

TEST(EventDataTests, LoadNullTerminated_WStringView_EmptyString)
{
    const wchar_t nullTerminator = L'\0';
    std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(&nullTerminator),
                              reinterpret_cast<const uint8_t*>(&nullTerminator) + sizeof(wchar_t));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result = eventData.LoadNullTerminated<std::wstring_view>();

    EXPECT_EQ(L"", result);
    EXPECT_EQ(0u, result.length());
}

TEST(EventDataTests, LoadNullTerminated_StringView_EmptyString)
{
    std::vector<uint8_t> data = { 0x00 };
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result = eventData.LoadNullTerminated<std::string_view>();

    EXPECT_EQ("", result);
    EXPECT_EQ(0u, result.length());
}

TEST(EventDataTests, LoadNullTerminated_IntType_UsesRead)
{
    uint32_t value = 0x12345678;
    std::vector<uint8_t> data(reinterpret_cast<uint8_t*>(&value),
                              reinterpret_cast<uint8_t*>(&value) + sizeof(value));
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result = eventData.LoadNullTerminated<uint32_t>();

    EXPECT_EQ(0x12345678u, result);
}

TEST(EventDataTests, LoadNullTerminated_WStringView_MultipleStrings)
{
    const wchar_t* str1 = L"First";
    const wchar_t* str2 = L"Second";
    std::vector<uint8_t> data;
    
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(str1),
                reinterpret_cast<const uint8_t*>(str1) + (wcslen(str1) + 1) * sizeof(wchar_t));
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(str2),
                reinterpret_cast<const uint8_t*>(str2) + (wcslen(str2) + 1) * sizeof(wchar_t));
    
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto result1 = eventData.LoadNullTerminated<std::wstring_view>();
    auto result2 = eventData.LoadNullTerminated<std::wstring_view>();

    EXPECT_EQ(L"First", result1);
    EXPECT_EQ(L"Second", result2);
}

TEST(EventDataTests, LoadNullTerminated_StringView_AdvancesReadPosition)
{
    std::vector<uint8_t> data;
    const char* str = "Test";
    uint32_t trailingValue = 0xCAFEBABE;
    
    data.insert(data.end(), str, str + strlen(str) + 1);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&trailingValue),
                reinterpret_cast<uint8_t*>(&trailingValue) + sizeof(trailingValue));
    
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto str_result = eventData.LoadNullTerminated<std::string_view>();
    auto value = eventData.ReadUint32();

    EXPECT_EQ("Test", str_result);
    EXPECT_EQ(0xCAFEBABE, value);
}

TEST(EventDataTests, LoadNullTerminated_WStringView_AdvancesReadPosition)
{
    std::vector<uint8_t> data;
    const wchar_t* str = L"WideTest";
    uint32_t trailingValue = 0xBEEFCAFE;
    
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(str),
                reinterpret_cast<const uint8_t*>(str) + (wcslen(str) + 1) * sizeof(wchar_t));
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&trailingValue),
                reinterpret_cast<uint8_t*>(&trailingValue) + sizeof(trailingValue));
    
    MockEventRecord mockRecord(data, EVENT_HEADER_FLAG_32_BIT_HEADER);

    DirectX::Etw::EventData eventData(mockRecord.Get());
    auto str_result = eventData.LoadNullTerminated<std::wstring_view>();
    auto value = eventData.ReadUint32();

    EXPECT_EQ(L"WideTest", str_result);
    EXPECT_EQ(0xBEEFCAFE, value);
}
