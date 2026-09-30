// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Error-handling helpers for DxTimingCaptureLibrary.
//
// The library reports unrecoverable parsing/state errors by throwing. It exposes
// the throwing helpers as Errors::ThrowToolException / ThrowFailure / ThrowIf.
//
// We leave some PIX code in here, to make it easy to replace these error helpers
// with PIX implementations.

#ifdef UsePixUtility

#include <PixUtility/include/PixUtility/Exception.h>

namespace DirectX::Etw::Errors
{
    using ::WinPix::ThrowToolException;
    using ::WinPix::ThrowFailure;
    using ::WinPix::ThrowIf;
}

#else

#include <exception>
#include <string>

#include <winerror.h> // HRESULT, FAILED, E_FAIL

namespace DirectX::Etw
{
    // Carries an HRESULT plus an optional human-readable message.
    class EtwException : public std::exception
    {
    public:
        explicit EtwException(HRESULT errorCode, const wchar_t* errorMessage = nullptr)
            : m_errorCode(errorCode)
        {
            if (errorMessage != nullptr)
            {
                m_errorMessage = errorMessage;
            }
        }

        HRESULT GetErrorCode() const noexcept
        {
            return m_errorCode;
        }

        const wchar_t* GetErrorMessage() const noexcept
        {
            return m_errorMessage.c_str();
        }

        const char* what() const noexcept override
        {
            return "DirectX::Etw::EtwException";
        }

    private:
        HRESULT m_errorCode;
        std::wstring m_errorMessage;
    };

    namespace Errors
    {
        [[noreturn]] inline void ThrowToolException(HRESULT errorCode, const wchar_t* errorMessage = nullptr)
        {
            throw EtwException(errorCode, errorMessage);
        }

        inline void ThrowFailure(HRESULT errorCode, const wchar_t* errorMessage = nullptr)
        {
            if (FAILED(errorCode))
            {
                ThrowToolException(errorCode, errorMessage);
            }
        }

        inline void ThrowIf(bool condition, HRESULT errorCode = E_FAIL, const wchar_t* errorMessage = nullptr)
        {
            if (condition)
            {
                ThrowToolException(errorCode, errorMessage);
            }
        }
    }
}

#endif
