#include "ThumbnailProvider.hpp"
#include <shobjidl.h>
#include <atomic>
#include <mutex>
#include <new>
#include <string>

namespace {
HMODULE module {};
std::atomic_ulong objects {0};
std::once_flag decoderOnce;
VulkanaReadThumbnailFunction decoder {};

void loadDecoder()
{
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    if (!length || length >= std::size(path)) return;
    std::wstring location(path, length);
    location.resize(location.find_last_of(L"\\/") + 1);
    location += L"vulkana-thumbnail-codec.dll";
    // No PATH/current-directory dependency search inside a Shell host. Qt and
    // minizip are private siblings, loaded only when an actual preview is asked
    // for. Never create a QApplication, start the editor or initialize Vulkan.
    const auto library = LoadLibraryExW(location.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library) decoder = reinterpret_cast<VulkanaReadThumbnailFunction>(GetProcAddress(library, "VulkanaReadThumbnail"));
    // Keep the codec loaded for the host's lifetime: Qt owns process-global
    // image-format state. Explorer's normal out-of-process isolation remains on.
}

class Thumbnail final : public IThumbnailProvider, public IInitializeWithStream {
    std::atomic_ulong refs_ {1};
    IStream* stream_ {};
public:
    Thumbnail() { ++objects; }
    ~Thumbnail() { if (stream_) stream_->Release(); --objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IThumbnailProvider)) *value = static_cast<IThumbnailProvider*>(this);
        else if (iid == __uuidof(IInitializeWithStream)) *value = static_cast<IInitializeWithStream*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE Initialize(IStream* stream, DWORD mode) override
    {
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        if (!stream) return E_INVALIDARG;
        if ((mode & 3) != STGM_READ) return STG_E_ACCESSDENIED;
        stream_ = stream; stream_->AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT size, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override
    {
        if (!bitmap || !alpha) return E_POINTER;
        *bitmap = nullptr; *alpha = WTSAT_UNKNOWN;
        if (!stream_ || !size || size > 4096) return E_INVALIDARG;
        try {
            std::call_once(decoderOnce, loadDecoder);
            return decoder ? decoder(stream_, size, bitmap, alpha) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        } catch (...) { return E_FAIL; }
    }
};
class Factory final : public IClassFactory {
    std::atomic_ulong refs_ {1};
public:
    Factory() { ++objects; }
    ~Factory() { --objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *value = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* object = new (std::nothrow) Thumbnail;
        if (!object) return E_OUTOFMEMORY;
        const auto result = object->QueryInterface(iid, value); object->Release(); return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) ++objects; else --objects; return S_OK; }
};
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) module = instance;
    return TRUE;
}
STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** value)
{
    if (!value) return E_POINTER;
    *value = nullptr;
    if (clsid != vulkanaThumbnailClsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) Factory;
    if (!factory) return E_OUTOFMEMORY;
    const auto result = factory->QueryInterface(iid, value); factory->Release(); return result;
}
STDAPI DllCanUnloadNow()
{
    // Once Qt is loaded, retain this module too so no subsequent activation
    // repeats codec initialization while old process-global Qt state survives.
    return objects == 0 && !decoder ? S_OK : S_FALSE;
}
