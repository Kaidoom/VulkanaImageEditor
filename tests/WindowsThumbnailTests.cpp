// Native COM probe: intentionally has no Qt application or DLL import.
#include "../src/project/windows/ThumbnailProvider.hpp"
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
using Microsoft::WRL::ComPtr;
int main(int argc, char**)
{
    int count {};
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args || argc < 3) return 2;
    const bool shell = count > 3 && std::wstring(args[3]) == L"--shell-cache";
    const bool bind = count > 3 && std::wstring(args[3]) == L"--shell-bind";
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 3;
    HRESULT result = E_FAIL;
    HBITMAP bitmap {};
    if (shell || bind) {
        ComPtr<IShellItem> item;
        result = SHCreateItemFromParsingName(args[2], nullptr, IID_PPV_ARGS(&item));
        std::cout << "Shell item: 0x" << std::hex << unsigned(result) << std::dec << '\n';
        if (bind) {
            ComPtr<IThumbnailProvider> provider;
            if (SUCCEEDED(result)) result = item->BindToHandler(nullptr, BHID_ThumbnailHandler, IID_PPV_ARGS(&provider));
            std::cout << "Shell binding: 0x" << std::hex << unsigned(result) << std::dec << '\n';
            WTS_ALPHATYPE alpha {};
            if (SUCCEEDED(result)) result = provider->GetThumbnail(128, &bitmap, &alpha);
        } else {
        ComPtr<IThumbnailCache> cache;
        if (SUCCEEDED(result)) result = CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache));
        std::cout << "Shell cache: 0x" << std::hex << unsigned(result) << std::dec << '\n';
        ComPtr<ISharedBitmap> shared;
        WTS_CACHEFLAGS flags {}; WTS_THUMBNAILID id {};
        if (SUCCEEDED(result)) result = cache->GetThumbnail(item.Get(), 128, WTS_FORCEEXTRACTION, &shared, &flags, &id);
        std::cout << "Shell extraction: 0x" << std::hex << unsigned(result) << std::dec << '\n';
        if (SUCCEEDED(result)) result = shared->Detach(&bitmap);
        }
    } else {
        const auto library = LoadLibraryExW(args[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto factoryFunction = library ? reinterpret_cast<HRESULT (WINAPI*)(REFCLSID, REFIID, void**)>(
            GetProcAddress(library, "DllGetClassObject")) : nullptr;
        ComPtr<IClassFactory> factory;
        result = factoryFunction ? factoryFunction(vulkanaThumbnailClsid, IID_PPV_ARGS(&factory)) : HRESULT_FROM_WIN32(GetLastError());
        ComPtr<IThumbnailProvider> provider;
        if (SUCCEEDED(result)) result = factory->CreateInstance(nullptr, IID_PPV_ARGS(&provider));
        ComPtr<IInitializeWithStream> initialize;
        if (SUCCEEDED(result)) result = provider.As(&initialize);
        ComPtr<IStream> stream;
        if (SUCCEEDED(result)) result = SHCreateStreamOnFileEx(args[2], STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
        if (SUCCEEDED(result)) result = initialize->Initialize(stream.Get(), STGM_READ);
        if (SUCCEEDED(result) && initialize->Initialize(stream.Get(), STGM_READ) != HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED)) result = E_FAIL;
        WTS_ALPHATYPE alpha {};
        if (SUCCEEDED(result)) result = provider->GetThumbnail(128, &bitmap, &alpha);
        if (SUCCEEDED(result) && alpha != WTSAT_ARGB) result = E_FAIL;
        if (SUCCEEDED(result)) {
            HBITMAP invalid {};
            if (provider->GetThumbnail(0, &invalid, &alpha) != E_INVALIDARG || invalid) result = E_FAIL;
        }
    }
    if (SUCCEEDED(result)) {
        BITMAP info {};
        const auto maximum = shell ? 256 : 128; // The Shell rounds 128 to its next cache tier.
        if (!bitmap || GetObjectW(bitmap, sizeof(info), &info) != sizeof(info)
            || info.bmWidth <= 0 || info.bmWidth > maximum || info.bmHeight <= 0 || info.bmHeight > maximum
            || info.bmBitsPixel != 32) result = E_FAIL;
        else std::cout << "Thumbnail " << info.bmWidth << 'x' << info.bmHeight << " BGRA32\n";
    }
    if (bitmap) DeleteObject(bitmap);
    std::cout << "HRESULT: 0x" << std::hex << unsigned(result) << '\n';
    CoUninitialize(); LocalFree(args);
    return SUCCEEDED(result) ? 0 : 1;
}
