#include "signatures.h"
#include <algorithm>

namespace signatures
{
    namespace
    {
        int hex_value(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        }

        bool is_whitespace(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }
    }

    bool parse_signature(std::string_view signature, std::vector<uint8_t>& pattern, std::vector<uint8_t>& mask)
    {
        pattern.clear();
        mask.clear();

        size_t pos = 0;
        while (pos < signature.size())
        {
            while (pos < signature.size() && is_whitespace(signature[pos]))
                ++pos;

            if (pos >= signature.size())
                break;

            size_t token_start = pos;
            while (pos < signature.size() && !is_whitespace(signature[pos]))
                ++pos;

            auto token = signature.substr(token_start, pos - token_start);
            if (token == "??" || token == "?")
            {
                pattern.push_back(0);
                mask.push_back(0);
            }
            else
            {
                if (token.empty() || token.size() > 2)
                    return false;

                uint8_t value = 0;
                for (char c : token)
                {
                    int nibble = hex_value(c);
                    if (nibble < 0)
                        return false;
                    value = static_cast<uint8_t>((value << 4) | static_cast<uint8_t>(nibble));
                }

                pattern.push_back(value);
                mask.push_back(1);
            }
        }

        return !pattern.empty();
    }

    uintptr_t scan_buffer(const uint8_t* data, size_t size,
                          const std::vector<uint8_t>& pattern, const std::vector<uint8_t>& mask)
    {
        if (pattern.empty() || pattern.size() != mask.size() || size < pattern.size())
            return UINTPTR_MAX;

        size_t end = size - pattern.size();
        for (size_t i = 0; i <= end; ++i)
        {
            bool found = true;
            for (size_t j = 0; j < pattern.size(); ++j)
            {
                if (mask[j] == 1 && data[i + j] != pattern[j])
                {
                    found = false;
                    break;
                }
            }
            if (found)
                return i;
        }

        return UINTPTR_MAX;
    }

    std::vector<uintptr_t> scan_buffer_all(const uint8_t* data, size_t size,
                                           const std::vector<uint8_t>& pattern, const std::vector<uint8_t>& mask)
    {
        std::vector<uintptr_t> results;
        if (pattern.empty() || pattern.size() != mask.size() || size < pattern.size())
            return results;

        size_t end = size - pattern.size();
        for (size_t i = 0; i <= end; ++i)
        {
            bool found = true;
            for (size_t j = 0; j < pattern.size(); ++j)
            {
                if (mask[j] == 1 && data[i + j] != pattern[j])
                {
                    found = false;
                    break;
                }
            }
            if (found)
                results.push_back(i);
        }

        return results;
    }

    ScanResult scan_process(HANDLE proc, uintptr_t base, size_t size, std::string_view signature)
    {
        auto all = scan_process_all(proc, base, size, signature);
        ScanResult result;
        result.match_count = all.size();
        if (!all.empty())
            result.address = all[0];
        return result;
    }

    std::vector<uintptr_t> scan_process_all(HANDLE proc, uintptr_t base, size_t size, std::string_view signature)
    {
        std::vector<uintptr_t> results;

        std::vector<uint8_t> pattern;
        std::vector<uint8_t> mask;
        if (!parse_signature(signature, pattern, mask))
            return results;

        // Parse PE headers to find executable sections (.text, etc.)
        // instead of scanning every committed page. Reduces ReadProcessMemory
        // calls and avoids reading non-code regions.
        uint8_t dos_buf[sizeof(IMAGE_DOS_HEADER)]{};
        SIZE_T rd = 0;
        if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(base), dos_buf, sizeof(dos_buf), &rd) || rd < sizeof(IMAGE_DOS_HEADER))
            return results;

        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(dos_buf);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return results;

        uintptr_t nt_addr = base + static_cast<uintptr_t>(dos->e_lfanew);
        uint8_t nt_buf[sizeof(IMAGE_NT_HEADERS64)]{};
        if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(nt_addr), nt_buf, sizeof(nt_buf), &rd) || rd < sizeof(IMAGE_NT_HEADERS64))
            return results;

        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(nt_buf);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return results;

        WORD num_sections = nt->FileHeader.NumberOfSections;
        uintptr_t section_table_addr = nt_addr + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
                                        + nt->FileHeader.SizeOfOptionalHeader;

        std::vector<IMAGE_SECTION_HEADER> sections(num_sections);
        if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(section_table_addr),
                               sections.data(), num_sections * sizeof(IMAGE_SECTION_HEADER), &rd))
            return results;

        // Scan only executable sections
        for (WORD i = 0; i < num_sections; ++i)
        {
            const auto& sec = sections[i];
            if (!(sec.Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;

            uintptr_t sec_base = base + static_cast<uintptr_t>(sec.VirtualAddress);
            size_t sec_size = static_cast<size_t>(sec.Misc.VirtualSize);
            if (sec_size == 0)
                continue;

            // Clamp to image bounds
            if (sec_base + sec_size > base + size)
                sec_size = static_cast<size_t>((base + size) - sec_base);

            std::vector<uint8_t> buffer(sec_size);
            SIZE_T bytes_read = 0;
            if (ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(sec_base),
                                  buffer.data(), sec_size, &bytes_read) && bytes_read > 0)
            {
                auto matches = scan_buffer_all(buffer.data(), static_cast<size_t>(bytes_read), pattern, mask);
                for (auto offset : matches)
                    results.push_back(sec_base + offset);
            }
        }

        return results;
    }
}
