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
constexpr wchar_t kInfMarker[] = L"Behringer BCD2000 DJ Controller";
constexpr wchar_t kLegacyInfMarker[] = L"Behringer BCD2000 (WinUSB)";
constexpr wchar_t kHwIdMatch[] = L"VID_1397&PID_00BD";
constexpr wchar_t kMetadataGuid[] = L"b2d20000-1397-00bd-a510-000000000064";

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
    wchar_t buf[1024]{};
    va_list args;
    va_start(args, fmt);
    std::vswprintf(buf, sizeof(buf) / sizeof(wchar_t), fmt, args);
    va_end(args);

    std::wprintf(L"%s\n", buf);
    std::fflush(stdout);

    wchar_t tempPath[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempPath) > 0) {
        std::wstring logFile = std::wstring(tempPath) + L"bcd2000_winusb_setup.log";
        FILE *f = _wfopen(logFile.c_str(), L"a, ccs=UTF-8");
        if (f) {
            std::fputws(buf, f);
            std::fputws(L"\n", f);
            std::fclose(f);
        }
    }
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
        "DriverVer   = 11/10/2026,2.0.0.0\r\n"
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
        "HKR,,DeviceDisplayIcon,0x00020000,\"%SystemRoot%\\System32\\bcd2000.ico\"\r\n"
        "HKR,,DeviceCategory,0x00010000,\"Audio.DJController\",\"Audio\"\r\n"
        "HKR,,FriendlyName,0x00000000,\"%DeviceName%\"\r\n"
        "\r\n"
        "[Strings]\r\n"
        "ManufacturerName = \"Behringer\"\r\n"
        "DeviceName       = \"Behringer BCD2000 DJ Controller\"\r\n";

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

    char osAttrVal[] = "2:6.0,2:6.1,2:6.2,2:6.3,2:10.0";
    char hwidVal[] = "usb\\vid_1397&pid_00bd";
    const DWORD attrFlags =
        CRYPTCAT_ATTR_AUTHENTICATED | CRYPTCAT_ATTR_NAMEASCII | CRYPTCAT_ATTR_DATAASCII;

    if (!CryptCATPutCatAttrInfo(
            hCat, const_cast<LPWSTR>(L"HWID1"), attrFlags,
            static_cast<DWORD>(std::strlen(hwidVal)),
            reinterpret_cast<BYTE *>(hwidVal)) ||
        !CryptCATPutCatAttrInfo(
            hCat, const_cast<LPWSTR>(L"OSAttr"), attrFlags,
            static_cast<DWORD>(std::strlen(osAttrVal)),
            reinterpret_cast<BYTE *>(osAttrVal))) {
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
        const_cast<LPWSTR>(kInfFileName),
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

    char infNameAscii[] = "bcd2000_winusb.inf";
    if (!CryptCATPutAttrInfo(
            hCat, member, const_cast<LPWSTR>(L"File"), attrFlags,
            static_cast<DWORD>(std::strlen(infNameAscii)),
            reinterpret_cast<BYTE *>(infNameAscii)) ||
        !CryptCATPutAttrInfo(
            hCat, member, const_cast<LPWSTR>(L"OSAttr"), attrFlags,
            static_cast<DWORD>(std::strlen(osAttrVal)),
            reinterpret_cast<BYTE *>(osAttrVal))) {
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

    DWORD provFlags = CRYPT_NEWKEYSET | CRYPT_MACHINE_KEYSET;
    DWORD delFlags = CRYPT_DELETEKEYSET | CRYPT_MACHINE_KEYSET;
    DWORD kpiFlags = CRYPT_MACHINE_KEYSET;

    // Scrub any leftover ephemeral container first
    HCRYPTPROV tmpProv = 0;
    CryptAcquireContextW(
        &tmpProv, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, delFlags);

    if (!CryptAcquireContextW(
            &outProv, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, provFlags)) {
        // If machine keyset is restricted, fall back to user keyset
        provFlags = CRYPT_NEWKEYSET;
        delFlags = CRYPT_DELETEKEYSET;
        kpiFlags = 0;
        CryptAcquireContextW(
            &tmpProv, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, delFlags);
        if (!CryptAcquireContextW(
                &outProv, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, provFlags)) {
            LogInfo(L"[ERR] CryptAcquireContextW(NEWKEYSET) failed (err=0x%lX)", GetLastError());
            return false;
        }
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

    // Add Basic Constraints: CA = TRUE (satisfies DriverStore root CA trust verification)
    CERT_BASIC_CONSTRAINTS2_INFO bc{};
    bc.fCA = TRUE;
    bc.fPathLenConstraint = FALSE;

    DWORD cbBc = 0;
    CryptEncodeObjectEx(
        X509_ASN_ENCODING, X509_BASIC_CONSTRAINTS2, &bc, 0, nullptr, nullptr, &cbBc);
    std::vector<BYTE> bcBytes(cbBc);
    if (!CryptEncodeObjectEx(
            X509_ASN_ENCODING, X509_BASIC_CONSTRAINTS2, &bc, 0, nullptr, bcBytes.data(), &cbBc)) {
        LogInfo(L"[ERR] CryptEncodeObjectEx(BC) failed (err=0x%lX)", GetLastError());
        return false;
    }

    // Add Key Usage: Digital Signature, Cert Sign, CRL Sign
    BYTE ku = CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE;
    CRYPT_BIT_BLOB kuBlob{};
    kuBlob.cbData = 1;
    kuBlob.pbData = &ku;

    DWORD cbKu = 0;
    CryptEncodeObjectEx(
        X509_ASN_ENCODING, X509_KEY_USAGE, &kuBlob, 0, nullptr, nullptr, &cbKu);
    std::vector<BYTE> kuBytes(cbKu);
    if (!CryptEncodeObjectEx(
            X509_ASN_ENCODING, X509_KEY_USAGE, &kuBlob, 0, nullptr, kuBytes.data(), &cbKu)) {
        LogInfo(L"[ERR] CryptEncodeObjectEx(KU) failed (err=0x%lX)", GetLastError());
        return false;
    }

    CERT_EXTENSION certExts[3]{};
    certExts[0].pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
    certExts[0].fCritical = FALSE;
    certExts[0].Value.cbData = cbEku;
    certExts[0].Value.pbData = ekuBytes.data();

    certExts[1].pszObjId = const_cast<LPSTR>(szOID_BASIC_CONSTRAINTS2);
    certExts[1].fCritical = TRUE;
    certExts[1].Value.cbData = cbBc;
    certExts[1].Value.pbData = bcBytes.data();

    certExts[2].pszObjId = const_cast<LPSTR>(szOID_KEY_USAGE);
    certExts[2].fCritical = TRUE;
    certExts[2].Value.cbData = cbKu;
    certExts[2].Value.pbData = kuBytes.data();

    CERT_EXTENSIONS exts{};
    exts.cExtension = 3;
    exts.rgExtension = certExts;

    CRYPT_KEY_PROV_INFO kpi{};
    kpi.pwszContainerName = const_cast<LPWSTR>(kKeyContainer);
    kpi.pwszProvName = const_cast<LPWSTR>(MS_ENH_RSA_AES_PROV_W);
    kpi.dwProvType = PROV_RSA_AES;
    kpi.dwFlags = kpiFlags;
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
        if (GetFileSizeEx(hFile, &sz) && sz.QuadPart > 0 && sz.QuadPart < 524288) {
            std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
            DWORD readBytes = 0;
            if (ReadFile(hFile, raw.data(), static_cast<DWORD>(raw.size()), &readBytes, nullptr)) {
                CloseHandle(hFile);
                hFile = INVALID_HANDLE_VALUE;

                // Match both ASCII/UTF-8 and UTF-16LE INF files cleanly
                std::string rawLower = raw;
                for (auto &c : rawLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

                bool match = false;
                if (rawLower.find("1397") != std::string::npos &&
                    rawLower.find("00bd") != std::string::npos) {
                    match = true;
                } else if (raw.size() >= 4) {
                    const wchar_t *wptr = reinterpret_cast<const wchar_t *>(raw.data());
                    size_t wlen = raw.size() / sizeof(wchar_t);
                    std::wstring wstr(wptr, wlen);
                    for (auto &wc : wstr) wc = towlower(wc);
                    if (wstr.find(L"1397") != std::wstring::npos &&
                        wstr.find(L"00bd") != std::wstring::npos) {
                        match = true;
                    }
                }

                if (match) {
                    LogInfo(L"[UNINSTALL] Found matching OEM INF: %s, removing...", fd.cFileName);
                    if (!SetupUninstallOEMInfW(fd.cFileName, SUOI_FORCEDELETE, nullptr)) {
                        LogInfo(L"[WARN] SetupUninstallOEMInfW(%s) err=%lu",
                                fd.cFileName, GetLastError());
                    } else {
                        LogInfo(L"[OK] Successfully uninstalled OEM INF %s", fd.cFileName);
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

void SetDeviceDisplayProperties() {
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return;
    }

    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    const wchar_t kFriendlyName[] = L"Behringer BCD2000 DJ Controller";
    const wchar_t kIconPath[] = L"%SystemRoot%\\System32\\bcd2000.ico";
    const wchar_t kMfg[] = L"Behringer";
    const wchar_t kCatMulti[] = L"Audio.DJController\0Audio\0";

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); ++i) {
        wchar_t hwidBuf[512]{};
        if (SetupDiGetDeviceRegistryPropertyW(
                hDevInfo, &devInfoData, SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<PBYTE>(hwidBuf), sizeof(hwidBuf), nullptr)) {
            std::wstring hwid(hwidBuf);
            if (hwid.find(L"VID_1397") != std::wstring::npos &&
                hwid.find(L"PID_00BD") != std::wstring::npos) {
                LogInfo(L"[INSTALL] Setting device display properties for %s...", hwid.c_str());

                SetupDiSetDeviceRegistryPropertyW(
                    hDevInfo, &devInfoData, SPDRP_FRIENDLYNAME,
                    reinterpret_cast<const BYTE *>(kFriendlyName),
                    static_cast<DWORD>((std::wcslen(kFriendlyName) + 1) * sizeof(wchar_t)));

                SetupDiSetDeviceRegistryPropertyW(
                    hDevInfo, &devInfoData, SPDRP_MFG,
                    reinterpret_cast<const BYTE *>(kMfg),
                    static_cast<DWORD>((std::wcslen(kMfg) + 1) * sizeof(wchar_t)));

                HKEY hDevKey = SetupDiOpenDevRegKey(
                    hDevInfo, &devInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_SET_VALUE);
                if (hDevKey != INVALID_HANDLE_VALUE && hDevKey != nullptr) {
                    RegSetValueExW(
                        hDevKey, L"FriendlyName", 0, REG_SZ,
                        reinterpret_cast<const BYTE *>(kFriendlyName),
                        static_cast<DWORD>((std::wcslen(kFriendlyName) + 1) * sizeof(wchar_t)));
                    RegSetValueExW(
                        hDevKey, L"DeviceDisplayIcon", 0, REG_EXPAND_SZ,
                        reinterpret_cast<const BYTE *>(kIconPath),
                        static_cast<DWORD>((std::wcslen(kIconPath) + 1) * sizeof(wchar_t)));
                    RegSetValueExW(
                        hDevKey, L"DeviceCategory", 0, REG_MULTI_SZ,
                        reinterpret_cast<const BYTE *>(kCatMulti),
                        static_cast<DWORD>(sizeof(kCatMulti)));
                    RegCloseKey(hDevKey);
                }

                // Also update hardware instance key & Device Parameters in registry
                HKEY hUsbRoot = nullptr;
                if (RegOpenKeyExW(
                        HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_1397&PID_00BD",
                        0, KEY_READ, &hUsbRoot) == ERROR_SUCCESS) {
                    wchar_t instName[256]{};
                    DWORD idx = 0;
                    DWORD nameLen = static_cast<DWORD>(std::size(instName));
                    while (RegEnumKeyExW(hUsbRoot, idx++, instName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                        std::wstring instPath = std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_1397&PID_00BD\\") + instName;
                        HKEY hInstKey = nullptr;
                        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, instPath.c_str(), 0, KEY_SET_VALUE, &hInstKey) == ERROR_SUCCESS) {
                            RegSetValueExW(
                                hInstKey, L"FriendlyName", 0, REG_SZ,
                                reinterpret_cast<const BYTE *>(kFriendlyName),
                                static_cast<DWORD>((std::wcslen(kFriendlyName) + 1) * sizeof(wchar_t)));
                            RegSetValueExW(
                                hInstKey, L"DeviceDesc", 0, REG_SZ,
                                reinterpret_cast<const BYTE *>(kFriendlyName),
                                static_cast<DWORD>((std::wcslen(kFriendlyName) + 1) * sizeof(wchar_t)));
                            RegSetValueExW(
                                hInstKey, L"Mfg", 0, REG_SZ,
                                reinterpret_cast<const BYTE *>(kMfg),
                                static_cast<DWORD>((std::wcslen(kMfg) + 1) * sizeof(wchar_t)));
                            RegCloseKey(hInstKey);
                        }

                        std::wstring paramPath = instPath + L"\\Device Parameters";
                        HKEY hParamKey = nullptr;
                        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, paramPath.c_str(), 0, KEY_SET_VALUE, &hParamKey) == ERROR_SUCCESS) {
                            RegSetValueExW(
                                hParamKey, L"FriendlyName", 0, REG_SZ,
                                reinterpret_cast<const BYTE *>(kFriendlyName),
                                static_cast<DWORD>((std::wcslen(kFriendlyName) + 1) * sizeof(wchar_t)));
                            RegSetValueExW(
                                hParamKey, L"DeviceDisplayIcon", 0, REG_EXPAND_SZ,
                                reinterpret_cast<const BYTE *>(kIconPath),
                                static_cast<DWORD>((std::wcslen(kIconPath) + 1) * sizeof(wchar_t)));
                            RegSetValueExW(
                                hParamKey, L"DeviceCategory", 0, REG_MULTI_SZ,
                                reinterpret_cast<const BYTE *>(kCatMulti),
                                static_cast<DWORD>(sizeof(kCatMulti)));
                            RegCloseKey(hParamKey);
                        }
                        nameLen = static_cast<DWORD>(std::size(instName));
                    }
                    RegCloseKey(hUsbRoot);
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
}

void InstallDeviceIconAndMetadata(PCCERT_CONTEXT cert) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    const size_t slashPos = exeDir.find_last_of(L"\\/");
    if (slashPos != std::wstring::npos) {
        exeDir = exeDir.substr(0, slashPos);
    }

    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);

    // 1. Copy bcd2000.ico to System32 if present
    std::wstring icoSrc = exeDir + L"\\bcd2000.ico";
    if (GetFileAttributesW(icoSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        icoSrc = exeDir + L"\\..\\installer\\resources\\bcd2000.ico";
    }
    std::wstring icoDst = std::wstring(sysDir) + L"\\bcd2000.ico";
    if (GetFileAttributesW(icoSrc.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CopyFileW(icoSrc.c_str(), icoDst.c_str(), FALSE)) {
            LogInfo(L"[INSTALL] Staged device icon to %s", icoDst.c_str());
        }
    }

    // 2. Stage signed Device Metadata Package to Windows DeviceMetadataStore
    wchar_t progData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH) > 0) {
        std::wstring metaStore = std::wstring(progData) + L"\\Microsoft\\Windows\\DeviceMetadataStore";
        std::wstring metaStoreEn = metaStore + L"\\en-US";
        CreateDirectoryW(metaStore.c_str(), nullptr);
        CreateDirectoryW(metaStoreEn.c_str(), nullptr);

        std::wstring metaPkgName = std::wstring(kMetadataGuid) + L".devicemetadata-ms";
        std::wstring metaSrc = exeDir + L"\\" + metaPkgName;
        if (GetFileAttributesW(metaSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
            metaSrc = exeDir + L"\\metadata\\" + metaPkgName;
        }
        if (GetFileAttributesW(metaSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
            metaSrc = exeDir + L"\\..\\installer\\metadata\\" + metaPkgName;
        }

        if (GetFileAttributesW(metaSrc.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::wstring dst1 = metaStore + L"\\" + metaPkgName;
            const std::wstring dst2 = metaStoreEn + L"\\" + metaPkgName;
            CopyFileW(metaSrc.c_str(), dst1.c_str(), FALSE);
            CopyFileW(metaSrc.c_str(), dst2.c_str(), FALSE);

            // Digitally sign the metadata packages with our trusted certificate so DMRC accepts them
            if (cert) {
                SignCatalogFile(dst1, cert);
                SignCatalogFile(dst2, cert);
            }
            LogInfo(L"[INSTALL] Registered and signed device metadata package %s", metaPkgName.c_str());
        }

        // Clean any old unsigned/failed cache so Windows regenerates it with the signed package
        std::wstring cacheDir = std::wstring(progData) +
            L"\\Microsoft\\Windows\\DeviceMetadataCache\\dmrccache\\en-us\\" + kMetadataGuid;
        DeleteFileW((cacheDir + L"\\PackageInfo.xml").c_str());
        DeleteFileW((cacheDir + L"\\DeviceInformation\\DeviceInfo.xml").c_str());
        DeleteFileW((cacheDir + L"\\DeviceInformation\\device.ico").c_str());
        DeleteFileW((cacheDir + L"\\WindowsInformation\\WindowsInfo.xml").c_str());
        RemoveDirectoryW((cacheDir + L"\\DeviceInformation").c_str());
        RemoveDirectoryW((cacheDir + L"\\WindowsInformation").c_str());
        RemoveDirectoryW(cacheDir.c_str());
    }
}

void UninstallDeviceIconAndMetadata() {
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    DeleteFileW((std::wstring(sysDir) + L"\\bcd2000.ico").c_str());

    wchar_t progData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH) > 0) {
        std::wstring metaStore = std::wstring(progData) + L"\\Microsoft\\Windows\\DeviceMetadataStore";
        std::wstring metaPkgName = std::wstring(kMetadataGuid) + L".devicemetadata-ms";
        DeleteFileW((metaStore + L"\\" + metaPkgName).c_str());
        DeleteFileW((metaStore + L"\\en-US\\" + metaPkgName).c_str());
    }
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
        InstallDeviceIconAndMetadata(cert);
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

    if (!signedOk) {
        LogInfo(L"[WARN] Catalog signing did not complete cleanly.");
    }
    const bool trustOk = VerifyCatalogTrust(catPath);
    LogInfo(L"[INSTALL] Catalog trust check: %s", trustOk ? L"VERIFIED" : L"UNVERIFIED");

    // Remove any stale copies of previous OEM INFs (including Zadig oem7.inf) before staging new one
    UninstallExistingOemInfs();

    // 1. Force update connected physical PnP device immediately
    BOOL needReboot = FALSE;
    HMODULE hNewdev = LoadLibraryW(L"newdev.dll");
    if (hNewdev) {
        auto pUpdateDriver = reinterpret_cast<BOOL(WINAPI *)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL)>(
            GetProcAddress(hNewdev, "UpdateDriverForPlugAndPlayDevicesW"));
        if (pUpdateDriver) {
            LogInfo(L"[INSTALL] Updating PnP device USB\\VID_1397&PID_00BD via UpdateDriverForPlugAndPlayDevicesW...");
            const BOOL upOk = pUpdateDriver(
                nullptr,
                L"USB\\VID_1397&PID_00BD",
                infPath.c_str(),
                INSTALLFLAG_FORCE,
                &needReboot);
            LogInfo(L"[INSTALL] UpdateDriverForPlugAndPlayDevicesW returned %d (needReboot=%d, err=0x%08lX)",
                    upOk ? 1 : 0, needReboot ? 1 : 0, GetLastError());
        }
        FreeLibrary(hNewdev);
    }

    // 2. Pre-install into DriverStore for future plug-and-play insertions
    LogInfo(L"[INSTALL] Pre-installing WinUSB driver via DiInstallDriverW...");
    const BOOL diOk = DiInstallDriverW(
        nullptr,
        infPath.c_str(),
        DIIRFLAG_FORCE_INF,
        &needReboot);
    const DWORD diErr = GetLastError();

    // 3. Set device display properties and icon directly on hardware registry keys
    SetDeviceDisplayProperties();

    CleanupDirectory(workDir);

    if (!diOk) {
        LogInfo(L"[WARN] DiInstallDriverW returned (err=0x%08lX / %lu)", diErr, diErr);
    }

    LogInfo(L"[OK] WinUSB driver staged and bound (needReboot=%d).", needReboot ? 1 : 0);
    return 0;
}

int RunUninstall() {
    UninstallExistingOemInfs();
    UninstallDeviceIconAndMetadata();
    RemoveMatchingCertsFromStore(L"Root");
    RemoveMatchingCertsFromStore(L"TrustedPublisher");
    LogInfo(L"[OK] Removed BCD2000 WinUSB OEM INF, metadata and local signing certificate.");
    return 0;
}

int RunTestCat(const std::wstring &targetDir) {
    CreateDirectoryW(targetDir.c_str(), nullptr);
    std::wstring infPath, catPath;
    const bool ok = BuildInfAndCatalog(targetDir, infPath, catPath);
    LogInfo(L"[TEST-CAT] result=%d inf=%s cat=%s", ok ? 1 : 0, infPath.c_str(), catPath.c_str());
    return ok ? 0 : 1;
}

int RunTestSign(const std::wstring &targetDir) {
    CreateDirectoryW(targetDir.c_str(), nullptr);
    std::wstring infPath, catPath;
    if (!BuildInfAndCatalog(targetDir, infPath, catPath)) {
        LogInfo(L"[TEST-SIGN] BuildInfAndCatalog failed");
        return 1;
    }
    PCCERT_CONTEXT cert = nullptr;
    HCRYPTPROV prov = 0;
    if (!CreateEphemeralSigningCert(cert, prov)) {
        LogInfo(L"[TEST-SIGN] CreateEphemeralSigningCert failed");
        return 2;
    }
    const bool rootOk = InstallCertToLocalMachineStore(cert, L"Root");
    const bool pubOk = InstallCertToLocalMachineStore(cert, L"TrustedPublisher");
    LogInfo(L"[TEST-SIGN] Install rootOk=%d pubOk=%d", rootOk ? 1 : 0, pubOk ? 1 : 0);

    const bool signedOk = SignCatalogFile(catPath, cert);
    LogInfo(L"[TEST-SIGN] SignCatalogFile ok=%d", signedOk ? 1 : 0);

    const bool trustOk = VerifyCatalogTrust(catPath);
    LogInfo(L"[TEST-SIGN] VerifyCatalogTrust ok=%d", trustOk ? 1 : 0);

    CertFreeCertificateContext(cert);
    if (prov) {
        CryptReleaseContext(prov, 0);
    }
    return (signedOk && trustOk) ? 0 : 3;
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
        if (std::wcscmp(argv[1], L"--test-sign") == 0) {
            std::wstring dir = (argc >= 3) ? argv[2] : CreateTempWorkDir();
            return RunTestSign(dir);
        }
    }
    return RunInstall();
}
