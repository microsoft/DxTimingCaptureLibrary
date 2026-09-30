// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <stdexcept>
#include <cstdint>
#include <intrin.h>

namespace DirectX::Etw
{
    inline uint64_t TicksToNanoseconds(uint64_t ticks, uint64_t frequency)
    {
        constexpr uint64_t NanosecondsPerSecond = 1000000000ull;

        // Compute (ticks * NanosecondsPerSecond) / frequency using 128-bit arithmetic
        uint64_t productHigh;
        uint64_t productLow;

#if defined(_M_X64)
        productLow = _umul128(ticks, NanosecondsPerSecond, &productHigh);
#elif defined(_M_ARM64)
        productLow = ticks * NanosecondsPerSecond;
        productHigh = __umulh(ticks, NanosecondsPerSecond);
#else
        #error "Unsupported architecture: TicksToNanoseconds requires x64 or ARM64"
#endif

        // Check for overflow before division
        if (productHigh >= frequency)
        {
            // If productHigh >= frequency, the quotient will not fit in 64 bits.
            throw std::overflow_error("Result exceeds uint64_t range");
        }

        uint64_t remainder;
#if defined(_M_X64)
        return _udiv128(productHigh, productLow, frequency, &remainder);
#elif defined(_M_ARM64)
        // _udiv128 is not generally available on ARM64 MSVC, perform valid 128/64 division manually.
        // We know the result fits in 64 bits because productHigh < frequency.
        uint64_t q = 0;
        uint64_t r = productHigh;

        for (int i = 63; i >= 0; --i)
        {
            uint64_t bit = (productLow >> i) & 1;
            uint64_t r_high = r >> 63; // Save MSB before shift to detect overflow
            
            r = (r << 1) | bit;

            // If we overflowed the high bit of r, OR if r >= frequency
            if (r_high || r >= frequency)
            {
                r -= frequency;
                q |= (1ULL << i);
            }
        }
        remainder = r;
        return q;
#else
        return 0; // Unreachable due to #error above
#endif
    }
} // namespace DirectX::Etw
