/**
 * @file bcd2000_winusb_setup.cpp
 * @brief Self-contained Windows 10/11 x64 WinUSB driver installer/uninstaller
 *        for the Behringer BCD2000 (VID_1397 & PID_00BD).
 *
 * Behringer BCD2000 64-bit ASIO and MIDI Driver Suite
 * Copyright (C) 2024-2026 Martin Revert <martinrevert@gmail.com> and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Uses Windows inbox winusb.inf + winusb.sys (no external WDK redistributables),
 * builds a SHA-256 .cat catalog via wintrust.dll (CryptCATCDFOpen), signs it
 * with an ephemeral local 2048-bit SHA-256 self-signed certificate installed
 * into LocalMachine\Root & TrustedPublisher (same model as Zadig / libwdi),
 * and stages/binds the driver via DiInstallDriverW.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wincrypt.h>
#include <mscat.h>
#include <mssip.h>
#include <wintrust.h>
#include <softpub.h>
#include <setupapi.h>
#include <newdev.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kCertSubject[] = L"CN=Behringer BCD2000 WinUSB Local Driver";
constexpr wchar_t kInfFileName[] = L"bcd2000_winusb.inf";
constexpr wchar_t kCatFileName[] = L"bcd2000_winusb.cat";
constexpr wchar_t kKeyContainer[] = L"BCD2000_WinUSB_Ephemeral_Key";
constexpr wchar_t kInfMarker[] = L"Behringer BCD2000 (WinUSB)";
constexpr wchar_t kHwIdMatch[] = L"VID_1397&PID_00BD";

// Structures exported dynamically from Mssign32.dll (not shipped in standard Windows SDK headers)
constexpr DWORD kSignerSubjectFile = 0x01;
constexpr DWORD kSignerCertStore = 0x02;
constexpr DWORD kSignerCertPolicyChain = 0x02;
constexpr DWORD kSignerNoAttr = 0x00;

struct SIGNER_FILE_INFO {
    DWORD cbSize;
    LPCWSTR pwszFileName;
    HANDLE hFile;
};

struct SIGNER_SUBJECT_INFO {
    DWORD cbSize;
    DWORD *pdwIndex;
    DWORD dwSubjectChoice;
    union {
        SIGNER_FILE_INFO *pSignerFileInfo;
        PVOID pSignerBlobInfo;
    };
};

struct SIGNER_CERT_STORE_INFO {
    DWORD cbSize;
    PCCERT_CONTEXT pSigningCert;
    DWORD dwCertPolicy;
    HCERTSTORE hCertStore;
};

struct SIGNER_CERT {
    DWORD cbSize;
    DWORD dwCertChoice;
    union {
        LPCWSTR pwszSpcFile;
        SIGNER_CERT_STORE_INFO *pCertStoreInfo;
        PVOID pSpcChainInfo;
    };
    HWND hwnd;
};

struct SIGNER_SIGNATURE_INFO {
    DWORD cbSize;
    ALG_ID algidHash;
    DWORD dwAttrChoice;
    union {
        PVOID pAttrAuthcode;
        PVOID pAttrMs;
    };
    PCRYPT_ATTRIBUTES psAuthenticated;
    PCRYPT_ATTRIBUTES psUnauthenticated;
};

struct SIGNER_CONTEXT {
    DWORD cbSize;
    DWORD cbBlob;
    BYTE *pbBlob;
};

using PFN_SignerSignEx2 = HRESULT(WINAPI *)(
    DWORD dwFlags,
    SIGNER_SUBJECT_INFO *pSubjectInfo,
    SIGNER_CERT *pSignerCert,
    SIGNER_SIGNATURE_INFO *pSignatureInfo,
    PVOID pProviderInfo,
    DWORD dwTimestampFlags,
    PCSTR pszTimestampAlgorithmOid,
    PCWSTR pwszHttpTimeStamp,
    PCRYPT_ATTRIBUTES psRequest,
    PVOID pSipData,
    SIGNER_CONTEXT **ppSignerContext,
    PCERT_STRONG_SIGN_PARA pCryptoPolicy,
    PVOID pReserved);

using PFN_SignerFreeSignerContext = HRESULT(WINAPI *)(SIGNER_CONTEXT *pSignerContext);

void LogInfo(const wchar_t *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vwprintf(fmt, args);
    va_end(args);
    std::wprintf(L"\n");
}

bool WriteUtf8File(const std::wstring &path, const std::string &content) {
    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(
        hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(hFile);
    return ok && written == content.size();
}

bool BuildInfAndCatalog(const std::wstring &workDir, std::wstring &outInfPath, std::wstring &outCatPath) {
    wchar_t absWorkDir[MAX_PATH]{};
    GetFullPathNameW(workDir.c_str(), MAX_PATH, absWorkDir, nullptr);
    const std::wstring fullWorkDir(absWorkDir);

    outInfPath = fullWorkDir + L"\\" + kInfFileName;
    outCatPath = fullWorkDir + L"\\" + kCatFileName;

    // 1. Write WinUSB .inf referencing inbox winusb.inf and keeping Zadig's generic interface GUID
    //    {DEE824EF-729B-4A0E-9C14-B7117D33A817} used by bcd2000_asio.dll and bcd2000_midi.dll.
    const std::string infContent =
        "; Behringer BCD2000 WinUSB Driver Package (Windows 10/11 x64)\r\n"
        "[Version]\r\n"
        "Signature   = \"$Windows NT$\"\r\n"
        "Class       = USBDevice\r\n"
        "ClassGuid   = {88BAE032-5A81-49F0-BC3D-A4FF138216D6}\r\n"
        "Provider    = %ManufacturerName%\r\n"
        "CatalogFile = bcd2000_winusb.cat\r\n"
        "DriverVer   = 11/10/2026,1.0.0.0\r\n"
        "\r\n"
        "[Manufacturer]\r\n"
        "%ManufacturerName% = Standard,NTamd64\r\n"
        "\r\n"
        "[Standard.NTamd64]\r\n"
        "%DeviceName% = USB_Install, USB\\VID_1397&PID_00BD\r\n"
        "\r\n"
        "[USB_Install]\r\n"
        "Include = winusb.inf\r\n"
        "Needs   = WINUSB.NT\r\n"
        "\r\n"
        "[USB_Install.Services]\r\n"
        "Include = winusb.inf\r\n"
        "Needs   = WINUSB.NT.Services\r\n"
        "\r\n"
        "[USB_Install.HW]\r\n"
        "AddReg = Dev_AddReg\r\n"
        "\r\n"
        "[Dev_AddReg]\r\n"
        "HKR,,DeviceInterfaceGUIDs,0x10000,\"{DEE824EF-729B-4A0E-9C14-B7117D33A817}\"\r\n"
        "\r\n"
        "[Strings]\r\n"
        "ManufacturerName = \"Behringer BCD2000 ASIO/MIDI Project\"\r\n"
        "DeviceName       = \"Behringer BCD2000 (WinUSB)\"\r\n";

    if (!WriteUtf8File(outInfPath, infContent)) {
        LogInfo(L"[ERR] Failed to write INF file: %s (err=%lu)", outInfPath.c_str(), GetLastError());
        return false;
    }

    // 2. Build SHA-256 catalog store directly via CryptCATOpen + CryptCATPutMemberInfo
    HANDLE hCat = CryptCATOpen(
        const_cast<LPWSTR>(outCatPath.c_str()),
        CRYPTCAT_OPEN_CREATENEW,
        0,
        CRYPTCAT_VERSION_2,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING);
    if (hCat == INVALID_HANDLE_VALUE || hCat == nullptr) {
        LogInfo(L"[ERR] CryptCATOpen failed (err=0x%lX)", GetLastError());
        return false;
    }

    const wchar_t osAttrVal[] = L"2:6.0,2:6.1,2:6.2,2:6.3,2:10.0";
    const wchar_t hwidVal[] = L"usb\\vid_1397&pid_00bd";
    const DWORD attrFlags =
        CRYPTCAT_ATTR_AUTHENTICATED | CRYPTCAT_ATTR_NAMEASCII | CRYPTCAT_ATTR_DATAASCII;

    if (!CryptCATPutCatAttrInfo(
            hCat, const_cast<LPWSTR>(L"HWID1"), attrFlags,
            static_cast<DWORD>(std::wcslen(hwidVal) * sizeof(wchar_t)),
            reinterpret_cast<BYTE *>(const_cast<LPWSTR>(hwidVal))) ||
        !CryptCATPutCatAttrInfo(
            hCat, const_cast<LPWSTR>(L"OSAttr"), attrFlags,
            static_cast<DWORD>(std::wcslen(osAttrVal) * sizeof(wchar_t)),
            reinterpret_cast<BYTE *>(const_cast<LPWSTR>(osAttrVal)))) {
        LogInfo(L"[ERR] CryptCATPutCatAttrInfo failed (err=0x%lX)", GetLastError());
        CryptCATClose(hCat);
        return false;
    }

    // Calculate SHA-256 catalog hash for INF reference tag
    HCATADMIN hCatAdmin = nullptr;
    if (!CryptCATAdminAcquireContext2(&hCatAdmin, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
        LogInfo(L"[ERR] CryptCATAdminAcquireContext2 failed (err=0x%lX)", GetLastError());
        CryptCATClose(hCat);
        return false;
    }

    HANDLE hInf = CreateFileW(
        outInfPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hInf == INVALID_HANDLE_VALUE) {
        LogInfo(L"[ERR] CreateFileW(%s) failed (err=%lu)", outInfPath.c_str(), GetLastError());
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        CryptCATClose(hCat);
        return false;
    }

    DWORD hashLen = 0;
    CryptCATAdminCalcHashFromFileHandle2(hCatAdmin, hInf, &hashLen, nullptr, 0);
    std::vector<BYTE> hashBytes(hashLen);
    if (hashLen == 0 || !CryptCATAdminCalcHashFromFileHandle2(hCatAdmin, hInf, &hashLen, hashBytes.data(), 0)) {
        LogInfo(L"[ERR] CryptCATAdminCalcHashFromFileHandle2 failed (err=0x%lX)", GetLastError());
        CloseHandle(hInf);
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        CryptCATClose(hCat);
        return false;
    }
    CloseHandle(hInf);
    CryptCATAdminReleaseContext(hCatAdmin, 0);

    std::wstring refTag;
    refTag.reserve(hashBytes.size() * 2);
    for (BYTE b : hashBytes) {
        wchar_t hex[3]{};
        std::swprintf(hex, 3, L"%02X", b);
        refTag += hex;
    }

    GUID subjGuid{};
    if (!CryptSIPRetrieveSubjectGuidForCatalogFile(outInfPath.c_str(), INVALID_HANDLE_VALUE, &subjGuid)) {
        LogInfo(L"[ERR] CryptSIPRetrieveSubjectGuidForCatalogFile failed (err=0x%lX)", GetLastError());
        CryptCATClose(hCat);
        return false;
    }

    SIP_SUBJECTINFO subjInfo{};
    subjInfo.cbSize = sizeof(subjInfo);
    subjInfo.pgSubjectType = &subjGuid;
    subjInfo.hFile = INVALID_HANDLE_VALUE;
    subjInfo.pwsFileName = outInfPath.c_str();
    subjInfo.DigestAlgorithm.pszObjId = const_cast<LPSTR>(szOID_NIST_sha256);
    subjInfo.dwFlags = SPC_INC_PE_RESOURCES_FLAG | SPC_INC_PE_IMPORT_ADDR_TABLE_FLAG | 0;
    subjInfo.dwEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

    DWORD cbIndirect = 0;
    if (!CryptSIPCreateIndirectData(&subjInfo, &cbIndirect, nullptr) || cbIndirect == 0) {
        LogInfo(L"[ERR] CryptSIPCreateIndirectData sizing failed (err=0x%lX)", GetLastError());
        CryptCATClose(hCat);
        return false;
    }
    std::vector<BYTE> indirectBuf(cbIndirect);
    auto *pIndirect = reinterpret_cast<SIP_INDIRECT_DATA *>(indirectBuf.data());
    if (!CryptSIPCreateIndirectData(&subjInfo, &cbIndirect, pIndirect)) {
        LogInfo(L"[ERR] CryptSIPCreateIndirectData failed (err=0x%lX)", GetLastError());
        CryptCATClose(hCat);
        return false;
    }

    CRYPTCATMEMBER *member = CryptCATPutMemberInfo(
        hCat,
        const_cast<LPWSTR>(outInfPath.c_str()),
        const_cast<LPWSTR>(refTag.c_str()),
        &subjGuid,
        0x200,
        cbIndirect,
        indirectBuf.data());
    if (!member) {
        LogInfo(L"[ERR] CryptCATPutMemberInfo failed (err=0x%lX)", GetLastError());
        CryptCATClose(hCat);
        return false;
    }

    if (!CryptCATPutAttrInfo(
            hCat, member, const_cast<LPWSTR>(L"File"), attrFlags,
            static_cast<DWORD>(std::wcslen(kInfFileName) * sizeof(wchar_t)),
            reinterpret_cast<BYTE *>(const_cast<LPWSTR>(kInfFileName))) ||
        !CryptCATPutAttrInfo(
            hCat, member, const_cast<LPWSTR>(L"OSAttr"), attrFlags,
            static_cast<DWORD>(std::wcslen(osAttrVal) * sizeof(wchar_t)),
            reinterpret_cast<BYTE *>(const_cast<LPWSTR>(osAttrVal)))) {
        LogInfo(L"[ERR] CryptCATPutAttrInfo failed (err=0x%lX)", GetLastError());
        CryptCATClose(hCat);
        return false;
    }

    if (!CryptCATPersistStore(hCat)) {
        LogInfo(L"[ERR] CryptCATPersistStore failed (err=0x%lX)", GetLastError());
        CryptCATClose(hCat);
        return false;
    }
    CryptCATClose(hCat);

    if (GetFileAttributesW(outCatPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogInfo(L"[ERR] Catalog file was not generated at %s", outCatPath.c_str());
        return false;
    }

    LogInfo(L"[OK] Generated INF and SHA-256 CAT at %s", fullWorkDir.c_str());
    return true;
}

bool CreateEphemeralSigningCert(PCCERT_CONTEXT &outCert, HCRYPTPROV &outProv) {
    outCert = nullptr;
    outProv = 0;

    // Scrub any leftover ephemeral container first
    HCRYPTPROV tmpProv = 0;
    CryptAcquireContextW(
        &tmpProv, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
        CRYPT_DELETEKEYSET | CRYPT_MACHINE_KEYSET);

    if (!CryptAcquireContextW(
            &outProv, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
            CRYPT_NEWKEYSET | CRYPT_MACHINE_KEYSET)) {
        LogInfo(L"[ERR] CryptAcquireContextW(NEWKEYSET) failed (err=0x%lX)", GetLastError());
        return false;
    }

    HCRYPTKEY hKey = 0;
    // RSA-2048 signature key
    if (!CryptGenKey(outProv, AT_SIGNATURE, (2048U << 16) | CRYPT_EXPORTABLE, &hKey)) {
        LogInfo(L"[ERR] CryptGenKey(RSA-2048) failed (err=0x%lX)", GetLastError());
        CryptReleaseContext(outProv, 0);
        outProv = 0;
        return false;
    }
    CryptDestroyKey(hKey);

    DWORD cbName = 0;
    if (!CertStrToNameW(
            X509_ASN_ENCODING, kCertSubject, CERT_X500_NAME_STR,
            nullptr, nullptr, &cbName, nullptr)) {
        LogInfo(L"[ERR] CertStrToNameW sizing failed (err=0x%lX)", GetLastError());
        return false;
    }
    std::vector<BYTE> encodedName(cbName);
    if (!CertStrToNameW(
            X509_ASN_ENCODING, kCertSubject, CERT_X500_NAME_STR,
            nullptr, encodedName.data(), &cbName, nullptr)) {
        LogInfo(L"[ERR] CertStrToNameW failed (err=0x%lX)", GetLastError());
        return false;
    }

    CERT_NAME_BLOB subjectBlob{};
    subjectBlob.cbData = cbName;
    subjectBlob.pbData = encodedName.data();

    // Add Enhanced Key Usage: Code Signing (1.3.6.1.5.5.7.3.3)
    LPSTR ekuOid = const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING);
    CERT_ENHKEY_USAGE eku{};
    eku.cUsageIdentifier = 1;
    eku.rgpszUsageIdentifier = &ekuOid;

    DWORD cbEku = 0;
    CryptEncodeObjectEx(
        X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, 0, nullptr, nullptr, &cbEku);
    std::vector<BYTE> ekuBytes(cbEku);
    if (!CryptEncodeObjectEx(
            X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, 0, nullptr, ekuBytes.data(), &cbEku)) {
        LogInfo(L"[ERR] CryptEncodeObjectEx(EKU) failed (err=0x%lX)", GetLastError());
        return false;
    }

    CERT_EXTENSION certExt{};
    certExt.pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
    certExt.fCritical = FALSE;
    certExt.Value.cbData = cbEku;
    certExt.Value.pbData = ekuBytes.data();

    CERT_EXTENSIONS exts{};
    exts.cExtension = 1;
    exts.rgExtension = &certExt;

    CRYPT_KEY_PROV_INFO kpi{};
    kpi.pwszContainerName = const_cast<LPWSTR>(kKeyContainer);
    kpi.pwszProvName = const_cast<LPWSTR>(MS_ENH_RSA_AES_PROV_W);
    kpi.dwProvType = PROV_RSA_AES;
    kpi.dwFlags = CRYPT_MACHINE_KEYSET;
    kpi.dwKeySpec = AT_SIGNATURE;

    CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
    sigAlg.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    SYSTEMTIME startTime{};
    GetSystemTime(&startTime);
    SYSTEMTIME endTime = startTime;
    endTime.wYear += 10;

    outCert = CertCreateSelfSignCertificate(
        outProv, &subjectBlob, 0, &kpi, &sigAlg, &startTime, &endTime, &exts);
    if (!outCert) {
        LogInfo(L"[ERR] CertCreateSelfSignCertificate failed (err=0x%lX)", GetLastError());
        return false;
    }
    return true;
}

void RemoveMatchingCertsFromStore(const wchar_t *storeName) {
    HCERTSTORE hStore = CertOpenStore(
        CERT_STORE_PROV_SYSTEM, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
        storeName);
    if (!hStore) {
        return;
    }

    while (true) {
        PCCERT_CONTEXT cur = nullptr;
        bool deletedAny = false;
        while ((cur = CertEnumCertificatesInStore(hStore, cur)) != nullptr) {
            wchar_t nameBuf[256]{};
            CertGetNameStringW(
                cur, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                nameBuf, static_cast<DWORD>(std::size(nameBuf)));
            if (std::wcscmp(nameBuf, L"Behringer BCD2000 WinUSB Local Driver") == 0) {
                CertDeleteCertificateFromStore(cur); // also frees cur
                deletedAny = true;
                break;
            }
        }
        if (!deletedAny) {
            break;
        }
    }
    CertCloseStore(hStore, 0);
}

bool InstallCertToLocalMachineStore(PCCERT_CONTEXT cert, const wchar_t *storeName) {
    HCERTSTORE hStore = CertOpenStore(
        CERT_STORE_PROV_SYSTEM, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE,
        storeName);
    if (!hStore) {
        LogInfo(L"[ERR] CertOpenStore(%s) failed (err=0x%lX)", storeName, GetLastError());
        return false;
    }

    const BOOL ok = CertAddCertificateContextToStore(
        hStore, cert, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
    if (!ok) {
        LogInfo(L"[ERR] CertAddCertificateContextToStore(%s) failed (err=0x%lX)",
                storeName, GetLastError());
    }
    CertCloseStore(hStore, 0);
    return ok != FALSE;
}

bool SignCatalogFile(const std::wstring &catPath, PCCERT_CONTEXT cert) {
    HMODULE hMssign32 = LoadLibraryW(L"Mssign32.dll");
    if (!hMssign32) {
        LogInfo(L"[ERR] LoadLibraryW(Mssign32.dll) failed (err=%lu)", GetLastError());
        return false;
    }

    auto pSignerSignEx2 = reinterpret_cast<PFN_SignerSignEx2>(
        GetProcAddress(hMssign32, "SignerSignEx2"));
    auto pSignerFreeSignerContext = reinterpret_cast<PFN_SignerFreeSignerContext>(
        GetProcAddress(hMssign32, "SignerFreeSignerContext"));

    if (!pSignerSignEx2) {
        LogInfo(L"[ERR] SignerSignEx2 not found in Mssign32.dll");
        FreeLibrary(hMssign32);
        return false;
    }

    SIGNER_FILE_INFO fileInfo{};
    fileInfo.cbSize = sizeof(fileInfo);
    fileInfo.pwszFileName = catPath.c_str();

    DWORD dwIndex = 0;
    SIGNER_SUBJECT_INFO subjectInfo{};
    subjectInfo.cbSize = sizeof(subjectInfo);
    subjectInfo.pdwIndex = &dwIndex;
    subjectInfo.dwSubjectChoice = kSignerSubjectFile;
    subjectInfo.pSignerFileInfo = &fileInfo;

    SIGNER_CERT_STORE_INFO certStoreInfo{};
    certStoreInfo.cbSize = sizeof(certStoreInfo);
    certStoreInfo.pSigningCert = cert;
    certStoreInfo.dwCertPolicy = kSignerCertPolicyChain;

    SIGNER_CERT signerCert{};
    signerCert.cbSize = sizeof(signerCert);
    signerCert.dwCertChoice = kSignerCertStore;
    signerCert.pCertStoreInfo = &certStoreInfo;

    SIGNER_SIGNATURE_INFO sigInfo{};
    sigInfo.cbSize = sizeof(sigInfo);
    sigInfo.algidHash = CALG_SHA_256;
    sigInfo.dwAttrChoice = kSignerNoAttr;

    SIGNER_CONTEXT *signerCtx = nullptr;
    const HRESULT hr = pSignerSignEx2(
        0,
        &subjectInfo,
        &signerCert,
        &sigInfo,
        nullptr,
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &signerCtx,
        nullptr,
        nullptr);

    if (signerCtx && pSignerFreeSignerContext) {
        pSignerFreeSignerContext(signerCtx);
    }
    FreeLibrary(hMssign32);

    if (FAILED(hr)) {
        LogInfo(L"[ERR] SignerSignEx2 failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    LogInfo(L"[OK] Signed catalog %s with SHA-256 Authenticode signature", catPath.c_str());
    return true;
}

bool VerifyCatalogTrust(const std::wstring &catPath) {
    WINTRUST_FILE_INFO fileData{};
    fileData.cbStruct = sizeof(fileData);
    fileData.pcwszFilePath = catPath.c_str();

    GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wtData{};
    wtData.cbStruct = sizeof(wtData);
    wtData.dwUIChoice = WTD_UI_NONE;
    wtData.fdwRevocationChecks = WTD_REVOKE_NONE;
    wtData.dwUnionChoice = WTD_CHOICE_FILE;
    wtData.dwStateAction = WTD_STATEACTION_VERIFY;
    wtData.pFile = &fileData;

    const LONG status = WinVerifyTrust(nullptr, &policyGuid, &wtData);
    wtData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policyGuid, &wtData);

    LogInfo(L"[VERIFY] WinVerifyTrust returned 0x%08lX", static_cast<unsigned long>(status));
    return status == ERROR_SUCCESS;
}

std::wstring CreateTempWorkDir() {
    wchar_t tempBase[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, tempBase);
    if (n == 0 || n >= MAX_PATH) {
        return L".";
    }
    std::wstring dir(tempBase);
    if (!dir.empty() && dir.back() == L'\\') {
        dir.pop_back();
    }
    dir += L"\\bcd2000_winusb_pkg";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void CleanupDirectory(const std::wstring &dir) {
    DeleteFileW((dir + L"\\" + kInfFileName).c_str());
    DeleteFileW((dir + L"\\" + kCatFileName).c_str());
    RemoveDirectoryW(dir.c_str());
}

void UninstallExistingOemInfs() {
    wchar_t winDir[MAX_PATH]{};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) == 0) {
        return;
    }
    std::wstring infDir = std::wstring(winDir) + L"\\INF";
    std::wstring pattern = infDir + L"\\oem*.inf";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        const std::wstring fullPath = infDir + L"\\" + fd.cFileName;
        HANDLE hFile = CreateFileW(
            fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            continue;
        }
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(hFile, &sz) && sz.QuadPart > 0 && sz.QuadPart < 262144) {
            std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
            DWORD readBytes = 0;
            if (ReadFile(hFile, raw.data(), static_cast<DWORD>(raw.size()), &readBytes, nullptr)) {
                CloseHandle(hFile);
                hFile = INVALID_HANDLE_VALUE;

                // Match both UTF-8/ASCII and UTF-16LE INF files cleanly via wide search
                std::wstring content;
                if (raw.size() >= 2 &&
                    static_cast<unsigned char>(raw[0]) == 0xFF &&
                    static_cast<unsigned char>(raw[1]) == 0xFE) {
                    const size_t wlen = (raw.size() - 2) / sizeof(wchar_t);
                    content.resize(wlen);
                    std::memcpy(content.data(), raw.data() + 2, wlen * sizeof(wchar_t));
                } else {
                    int wlen = MultiByteToWideChar(
                        CP_ACP, 0, raw.data(), static_cast<int>(readBytes), nullptr, 0);
                    if (wlen > 0) {
                        content.resize(static_cast<size_t>(wlen));
                        MultiByteToWideChar(
                            CP_ACP, 0, raw.data(), static_cast<int>(readBytes), content.data(), wlen);
                    }
                }

                if (content.find(kInfMarker) != std::wstring::npos &&
                    content.find(kHwIdMatch) != std::wstring::npos) {
                    LogInfo(L"[UNINSTALL] Removing OEM INF: %s", fd.cFileName);
                    if (!SetupUninstallOEMInfW(fd.cFileName, SUOI_FORCEDELETE, nullptr)) {
                        LogInfo(L"[WARN] SetupUninstallOEMInfW(%s) err=%lu",
                                fd.cFileName, GetLastError());
                    }
                }
            }
        }
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

int RunInstall() {
    const std::wstring workDir = CreateTempWorkDir();
    std::wstring infPath, catPath;
    if (!BuildInfAndCatalog(workDir, infPath, catPath)) {
        CleanupDirectory(workDir);
        return 1;
    }

    PCCERT_CONTEXT cert = nullptr;
    HCRYPTPROV prov = 0;
    if (!CreateEphemeralSigningCert(cert, prov)) {
        CleanupDirectory(workDir);
        return 2;
    }

    // Remove previous local self-signed driver certs from this installer before adding current
    RemoveMatchingCertsFromStore(L"Root");
    RemoveMatchingCertsFromStore(L"TrustedPublisher");

    const bool rootOk = InstallCertToLocalMachineStore(cert, L"Root");
    const bool pubOk = InstallCertToLocalMachineStore(cert, L"TrustedPublisher");

    bool signedOk = false;
    if (rootOk && pubOk) {
        signedOk = SignCatalogFile(catPath, cert);
    }

    // Scrub private key container immediately after signing
    CertFreeCertificateContext(cert);
    if (prov) {
        CryptReleaseContext(prov, 0);
        HCRYPTPROV delProv = 0;
        CryptAcquireContextW(
            &delProv, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
            CRYPT_DELETEKEYSET | CRYPT_MACHINE_KEYSET);
    }

    if (!signedOk || !VerifyCatalogTrust(catPath)) {
        LogInfo(L"[ERR] Signing or trust verification failed.");
        CleanupDirectory(workDir);
        return 3;
    }

    // Remove any stale copies of our own previous OEM INF before staging the new one
    UninstallExistingOemInfs();

    LogInfo(L"[INSTALL] Installing WinUSB driver via DiInstallDriverW...");
    BOOL needReboot = FALSE;
    const BOOL diOk = DiInstallDriverW(
        nullptr,
        infPath.c_str(),
        DIIRFLAG_FORCE_INF,
        &needReboot);
    const DWORD diErr = GetLastError();

    CleanupDirectory(workDir);

    if (!diOk) {
        LogInfo(L"[ERR] DiInstallDriverW failed (err=0x%08lX / %lu)", diErr, diErr);
        return 4;
    }

    LogInfo(L"[OK] WinUSB driver staged and bound (needReboot=%d).", needReboot ? 1 : 0);
    return 0;
}

int RunUninstall() {
    UninstallExistingOemInfs();
    RemoveMatchingCertsFromStore(L"Root");
    RemoveMatchingCertsFromStore(L"TrustedPublisher");
    LogInfo(L"[OK] Removed BCD2000 WinUSB OEM INF and local signing certificate.");
    return 0;
}

int RunTestCat(const std::wstring &targetDir) {
    CreateDirectoryW(targetDir.c_str(), nullptr);
    std::wstring infPath, catPath;
    const bool ok = BuildInfAndCatalog(targetDir, infPath, catPath);
    LogInfo(L"[TEST-CAT] result=%d inf=%s cat=%s", ok ? 1 : 0, infPath.c_str(), catPath.c_str());
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    LogInfo(L"Behringer BCD2000 WinUSB Setup - Copyright (C) 2024-2026 Martin Revert and contributors");
    LogInfo(L"Licensed under GNU GPL v3 or later. This is free software with ABSOLUTELY NO WARRANTY.");
    if (argc >= 2) {
        if (std::wcscmp(argv[1], L"--uninstall") == 0 ||
            std::wcscmp(argv[1], L"/uninstall") == 0) {
            return RunUninstall();
        }
        if (std::wcscmp(argv[1], L"--test-cat") == 0) {
            std::wstring dir = (argc >= 3) ? argv[2] : CreateTempWorkDir();
            return RunTestCat(dir);
        }
    }
    return RunInstall();
}
