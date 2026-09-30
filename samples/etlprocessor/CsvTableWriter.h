// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

namespace EtlProcessor
{
    // Writes rows to a wide stream as comma-separated values, quoting any cell
    // that contains a comma, quote, or newline.
    class CsvTableWriter
    {
        std::wostream& m_stream;

    public:
        explicit CsvTableWriter(std::wostream& stream)
            : m_stream(stream)
        {
        }

        void AddRow(std::vector<std::wstring> cells)
        {
            for (auto& cell : cells)
            {
                Escape(cell);
            }

            for (size_t i = 0; i < cells.size(); ++i)
            {
                m_stream << cells[i];
                if (i + 1 < cells.size())
                {
                    m_stream << L",";
                }
            }

            m_stream << L"\n";
        }

    private:
        static void Escape(std::wstring& cell)
        {
            if (cell.find_first_of(L"\",\n") == std::wstring::npos)
            {
                return;
            }

            std::wstring escaped = L"\"";
            for (wchar_t c : cell)
            {
                // A literal quote is doubled inside a quoted field.
                if (c == L'"')
                {
                    escaped += L'"';
                }
                escaped += c;
            }
            escaped += L'"';

            cell = std::move(escaped);
        }
    };
}
