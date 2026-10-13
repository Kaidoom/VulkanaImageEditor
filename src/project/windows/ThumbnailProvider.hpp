#pragma once
#include <windows.h>
#include <objidl.h>
#include <thumbcache.h>

// Stable COM identity. Never reuse it for an incompatible interface.
inline constexpr CLSID vulkanaThumbnailClsid =
    {0x4ee78b14, 0x4465, 0x40f5, {0xbc, 0x5e, 0xc4, 0xf0, 0x96, 0xa5, 0x1b, 0x7f}};
inline constexpr wchar_t vulkanaThumbnailClsidText[] = L"{4EE78B14-4465-40F5-BC5E-C4F096A51B7F}";
using VulkanaReadThumbnailFunction = HRESULT (WINAPI*)(IStream*, UINT, HBITMAP*, WTS_ALPHATYPE*);
