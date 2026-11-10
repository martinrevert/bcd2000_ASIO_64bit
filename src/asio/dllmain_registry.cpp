/**
 * @file dllmain_registry.cpp
 * @brief In-Process 64-bit COM Server Entry Points, Class Factory & Windows Registry Installer
 *        for Behringer BCD2000 ASIO 64-bit (CLSID {B2D20000-1397-00BD-A510-000000000064}).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <unknwn.h>
#include <atomic>
#include <cwchar>

#include "bcd2000_asio_driver.h"

HMODULE g_hModule = nullptr;
std::atomic<long> g_ActiveComObjects{0};
std::atomic<long> g_ServerLocks{0};

namespace {

constexpr const wchar_t *kAsioClsidString   = L"{B2D20000-1397-00BD-A510-000000000064}";
constexpr const wchar_t *kAsioDriverName    = L"Behringer BCD2000 ASIO 64-bit";
constexpr const wchar_t *kAsioSubKeyPath    = L"SOFTWARE\\ASIO\\Behringer BCD2000 ASIO 64-bit";
constexpr const wchar_t *kClsidSubKeyPath   = L"SOFTWARE\\Classes\\CLSID\\{B2D20000-1397-00BD-A510-000000000064}";
constexpr const wchar_t *kInprocSubKeyPath  = L"SOFTWARE\\Classes\\CLSID\\{B2D20000-1397-00BD-A510-000000000064}\\InprocServer32";
constexpr const wchar_t *kThreadingModel    = L"Apartment";

class Bcd2000ClassFactory final : public IClassFactory {
public:
    Bcd2000ClassFactory() = default;
    virtual ~Bcd2000ClassFactory() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++m_refCount;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = --m_refCount;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    STDMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }

        Bcd2000AsioDriver *driver = new (std::nothrow) Bcd2000AsioDriver();
        if (!driver) {
            return E_OUTOFMEMORY;
        }

        HRESULT hr = driver->QueryInterface(riid, ppvObject);
        driver->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) {
            ++g_ServerLocks;
        } else {
            --g_ServerLocks;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refCount{1};
};

bool SetRegStringValue(HKEY hRootKey, const wchar_t *subKey, const wchar_t *valueName, const wchar_t *data) {
    HKEY hKey = nullptr;
    LONG status = RegCreateKeyExW(
        hRootKey,
        subKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE | KEY_WOW64_64KEY,
        nullptr,
        &hKey,
        nullptr
    );
    if (status != ERROR_SUCCESS) {
        return false;
    }

    const DWORD byteLen = static_cast<DWORD>(( std::wcslen(data) + 1 ) * sizeof(wchar_t));
    status = RegSetValueExW(
        hKey,
        valueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE *>(data),
        byteLen
    );
    RegCloseKey(hKey);
    return (status == ERROR_SUCCESS);
}

bool RegisterInRootHive(HKEY hRootKey, const wchar_t *dllPath) {
    // 1. Register COM Class: SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}
    if (!SetRegStringValue(hRootKey, kClsidSubKeyPath, nullptr, kAsioDriverName)) {
        return false;
    }
    if (!SetRegStringValue(hRootKey, kInprocSubKeyPath, nullptr, dllPath)) {
        return false;
    }
    if (!SetRegStringValue(hRootKey, kInprocSubKeyPath, L"ThreadingModel", kThreadingModel)) {
        return false;
    }

    // 2. Register Steinberg ASIO Driver: SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit
    if (!SetRegStringValue(hRootKey, kAsioSubKeyPath, L"CLSID", kAsioClsidString)) {
        return false;
    }
    if (!SetRegStringValue(hRootKey, kAsioSubKeyPath, L"Description", kAsioDriverName)) {
        return false;
    }

    return true;
}

void UnregisterInRootHive(HKEY hRootKey) {
    RegDeleteKeyExW(hRootKey, kInprocSubKeyPath, KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(hRootKey, kClsidSubKeyPath, KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(hRootKey, kAsioSubKeyPath, KEY_WOW64_64KEY, 0);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (!IsEqualCLSID(rclsid, CLSID_BCD2000_ASIO)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    Bcd2000ClassFactory *factory = new (std::nothrow) Bcd2000ClassFactory();
    if (!factory) {
        return E_OUTOFMEMORY;
    }

    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow(void) {
    return (g_ActiveComObjects.load() == 0 && g_ServerLocks.load() == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllRegisterServer(void) {
    wchar_t dllPath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hModule, dllPath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Register in HKEY_CURRENT_USER (always succeeds even without Administrator elevation,
    // and maps into HKEY_CLASSES_ROOT\CLSID automatically for CoCreateInstance)
    const bool hkcuOk = RegisterInRootHive(HKEY_CURRENT_USER, dllPath);

    // Register in HKEY_LOCAL_MACHINE (succeeds when elevated as Administrator)
    const bool hklmOk = RegisterInRootHive(HKEY_LOCAL_MACHINE, dllPath);

    if (hklmOk || hkcuOk) {
        return S_OK;
    }
    return E_ACCESSDENIED;
}

extern "C" HRESULT __stdcall DllUnregisterServer(void) {
    UnregisterInRootHive(HKEY_LOCAL_MACHINE);
    UnregisterInRootHive(HKEY_CURRENT_USER);
    return S_OK;
}
