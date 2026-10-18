// SsimSetup.dll: custom actions for the SSIM MSI.
//
//   InstallHidHide   immediate, UI sequence: runs the bundled HidHide installer (asks for admin itself)
//   SignAndInstall   deferred, as SYSTEM: signs the WinUHid driver with a certificate made on this PC,
//                    trusts that certificate, deletes its private key, installs the driver and the
//                    Root\WinUHid device
//   RemoveAll        deferred, as SYSTEM: removes the device, the driver package and the certificate,
//                    and %ProgramData%\SSIM when asked to (also the rollback of SignAndInstall)
//
// There is no code-signing certificate for SSIM, so each PC signs its own copy of the driver. The
// private key only exists for the few milliseconds the signing takes, so nothing else can ever be
// signed with it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <wincrypt.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <initguid.h>
#include <devpkey.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "msi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "rpcrt4.lib")

namespace fs = std::filesystem;

static const wchar_t kCertSubject[] = L"CN=SSIM Local Driver Signing";
static const wchar_t kCertName[] = L"SSIM Local Driver Signing";
static const wchar_t kWinUHidHwIds[] = L"Root\\WinUHid\0";
static const wchar_t kDriverInf[] = L"WinUHidDriver.inf";

// ---------------------------------------------------------------------------
// Logging into the MSI log (msiexec /l*v)
// ---------------------------------------------------------------------------

static MSIHANDLE g_msi;

static void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    std::wstring line = std::wstring(L"SsimSetup: ") + buf;
    PMSIHANDLE rec = MsiCreateRecord(1);
    MsiRecordSetStringW(rec, 0, line.c_str());
    MsiProcessMessage(g_msi, INSTALLMESSAGE_INFO, rec);
}

static std::wstring GetProp(const wchar_t* name)
{
    DWORD len = 0;
    wchar_t empty[1] = L"";
    MsiGetPropertyW(g_msi, name, empty, &len);
    std::wstring s(++len, L'\0');
    MsiGetPropertyW(g_msi, name, s.data(), &len);
    s.resize(len);
    return s;
}

// CustomActionData is "key=value;key=value".
static std::wstring DataValue(const std::wstring& data, const wchar_t* key)
{
    std::wstring k = std::wstring(key) + L"=";
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find(L';', pos);
        if (end == std::wstring::npos) end = data.size();
        if (data.compare(pos, k.size(), k) == 0) return data.substr(pos + k.size(), end - pos - k.size());
        pos = end + 1;
    }
    return L"";
}

// ---------------------------------------------------------------------------
// mssign32.dll (part of Windows; it is what signtool uses). Its structures are
// documented on learn.microsoft.com but not in any SDK header.
// ---------------------------------------------------------------------------

struct SIGNER_FILE_INFO { DWORD cbSize; LPCWSTR pwszFileName; HANDLE hFile; };
struct SIGNER_SUBJECT_INFO { DWORD cbSize; DWORD* pdwIndex; DWORD dwSubjectChoice; SIGNER_FILE_INFO* pSignerFileInfo; };
struct SIGNER_CERT_STORE_INFO { DWORD cbSize; PCCERT_CONTEXT pSigningCert; DWORD dwCertPolicy; HCERTSTORE hCertStore; };
struct SIGNER_CERT { DWORD cbSize; DWORD dwCertChoice; SIGNER_CERT_STORE_INFO* pCertStoreInfo; HWND hwnd; };
struct SIGNER_ATTR_AUTHCODE { DWORD cbSize; BOOL fCommercial; BOOL fIndividual; LPCWSTR pwszName; LPCWSTR pwszInfo; };
struct SIGNER_SIGNATURE_INFO {
    DWORD cbSize; ALG_ID algidHash; DWORD dwAttrChoice; SIGNER_ATTR_AUTHCODE* pAttrAuthcode;
    PCRYPT_ATTRIBUTES psAuthenticated; PCRYPT_ATTRIBUTES psUnauthenticated;
};
struct SIGNER_CONTEXT { DWORD cbSize; DWORD cbBlob; BYTE* pbBlob; };
constexpr DWORD SIGNER_SUBJECT_FILE = 1;
constexpr DWORD SIGNER_CERT_STORE = 2;
constexpr DWORD SIGNER_CERT_POLICY_CHAIN = 2;
constexpr DWORD SIGNER_AUTHCODE_ATTR = 1;

using SignerSignEx2Fn = HRESULT(WINAPI*)(DWORD, SIGNER_SUBJECT_INFO*, SIGNER_CERT*, SIGNER_SIGNATURE_INFO*, void*,
                                         DWORD, PCSTR, PCWSTR, PCRYPT_ATTRIBUTES, void*, SIGNER_CONTEXT**,
                                         void*, void*);
using SignerFreeSignerContextFn = HRESULT(WINAPI*)(SIGNER_CONTEXT*);

static bool SignFile(PCCERT_CONTEXT cert, const fs::path& file)
{
    static HMODULE dll = LoadLibraryExW(L"mssign32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto sign = dll ? (SignerSignEx2Fn)GetProcAddress(dll, "SignerSignEx2") : nullptr;
    auto freeCtx = dll ? (SignerFreeSignerContextFn)GetProcAddress(dll, "SignerFreeSignerContext") : nullptr;
    if (!sign || !freeCtx) return Log(L"mssign32.dll is not available (%lu)", GetLastError()), false;

    DWORD index = 0;
    SIGNER_FILE_INFO fileInfo = {sizeof(fileInfo), file.c_str(), nullptr};
    SIGNER_SUBJECT_INFO subject = {sizeof(subject), &index, SIGNER_SUBJECT_FILE, &fileInfo};
    SIGNER_CERT_STORE_INFO storeInfo = {sizeof(storeInfo), cert, SIGNER_CERT_POLICY_CHAIN, nullptr};
    SIGNER_CERT signerCert = {sizeof(signerCert), SIGNER_CERT_STORE, &storeInfo, nullptr};
    SIGNER_ATTR_AUTHCODE authcode = {sizeof(authcode), FALSE, TRUE, kCertName, nullptr};
    SIGNER_SIGNATURE_INFO sigInfo = {sizeof(sigInfo), CALG_SHA_256, SIGNER_AUTHCODE_ATTR, &authcode, nullptr, nullptr};
    SIGNER_CONTEXT* ctx = nullptr;
    HRESULT hr = sign(0, &subject, &signerCert, &sigInfo, nullptr, 0, nullptr, nullptr, nullptr, nullptr, &ctx,
                      nullptr, nullptr);
    if (ctx) freeCtx(ctx);
    if (FAILED(hr)) return Log(L"Signing %ls failed (0x%08lX)", file.c_str(), hr), false;
    Log(L"Signed %ls", file.filename().c_str());
    return true;
}

// ---------------------------------------------------------------------------
// The per-PC certificate
// ---------------------------------------------------------------------------

// Key container that exists only while signing.
struct SigningKey {
    std::wstring container;
    HCRYPTPROV prov = 0;

    bool Create()
    {
        UUID id;
        UuidCreate(&id);
        RPC_WSTR s = nullptr;
        UuidToStringW(&id, &s);
        container = L"SSIM-driver-signing-" + std::wstring((wchar_t*)s);
        RpcStringFreeW(&s);
        if (!CryptAcquireContextW(&prov, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                                  CRYPT_NEWKEYSET | CRYPT_MACHINE_KEYSET | CRYPT_SILENT))
            return Log(L"Creating the key container failed (0x%08lX)", GetLastError()), false;
        HCRYPTKEY key = 0;
        if (!CryptGenKey(prov, AT_SIGNATURE, (2048u << 16), &key))
            return Log(L"Creating the signing key failed (0x%08lX)", GetLastError()), false;
        CryptDestroyKey(key);
        return true;
    }

    ~SigningKey()
    {
        if (prov) CryptReleaseContext(prov, 0);
        if (!container.empty()) {
            HCRYPTPROV p = 0;
            if (CryptAcquireContextW(&p, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                                     CRYPT_DELETEKEYSET | CRYPT_MACHINE_KEYSET | CRYPT_SILENT))
                Log(L"Deleted the signing key");
            else
                Log(L"Deleting the signing key failed (0x%08lX)", GetLastError());
        }
    }
};

static PCCERT_CONTEXT CreateCertificate(const SigningKey& key)
{
    BYTE nameBuf[256];
    DWORD nameLen = sizeof(nameBuf);
    if (!CertStrToNameW(X509_ASN_ENCODING, kCertSubject, CERT_X500_NAME_STR, nullptr, nameBuf, &nameLen, nullptr))
        return Log(L"CertStrToName failed (0x%08lX)", GetLastError()), nullptr;
    CERT_NAME_BLOB name = {nameLen, nameBuf};

    // Extended key usage: code signing.
    LPSTR usages[] = {(LPSTR)szOID_PKIX_KP_CODE_SIGNING};
    CERT_ENHKEY_USAGE eku = {1, usages};
    BYTE ekuBuf[64];
    DWORD ekuLen = sizeof(ekuBuf);
    CryptEncodeObject(X509_ASN_ENCODING, szOID_ENHANCED_KEY_USAGE, &eku, ekuBuf, &ekuLen);
    // Key usage: digital signature only (same extensions as New-SelfSignedCertificate -Type CodeSigningCert).
    BYTE kuBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
    CRYPT_BIT_BLOB ku = {1, &kuBits, 7};
    BYTE kuBuf[16];
    DWORD kuLen = sizeof(kuBuf);
    CryptEncodeObject(X509_ASN_ENCODING, szOID_KEY_USAGE, &ku, kuBuf, &kuLen);
    CERT_EXTENSION exts[] = {{(LPSTR)szOID_KEY_USAGE, TRUE, {kuLen, kuBuf}},
                             {(LPSTR)szOID_ENHANCED_KEY_USAGE, FALSE, {ekuLen, ekuBuf}}};
    CERT_EXTENSIONS extensions = {ARRAYSIZE(exts), exts};

    CRYPT_KEY_PROV_INFO provInfo = {(LPWSTR)key.container.c_str(), (LPWSTR)MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                                    CRYPT_MACHINE_KEYSET, 0, nullptr, AT_SIGNATURE};
    CRYPT_ALGORITHM_IDENTIFIER alg = {(LPSTR)szOID_RSA_SHA256RSA, {}};

    // The driver signature has no timestamp, so the certificate must outlive the installation.
    SYSTEMTIME start, end;
    GetSystemTime(&start);
    start.wYear -= 1;  // tolerate a wrong clock
    end = start;
    end.wYear += 51;

    PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(key.prov, &name, 0, &provInfo, &alg, &start, &end, &extensions);
    if (!cert) Log(L"CertCreateSelfSignCertificate failed (0x%08lX)", GetLastError());
    return cert;
}

static bool AddToStore(const wchar_t* storeName, PCCERT_CONTEXT cert)
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, storeName);
    if (!store) return Log(L"Opening LocalMachine\\%ls failed (0x%08lX)", storeName, GetLastError()), false;
    // Add the encoded certificate only, so the store copy carries no link to the private key.
    BOOL ok = CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, cert->pbCertEncoded, cert->cbCertEncoded,
                                               CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
    if (!ok) Log(L"Adding the certificate to LocalMachine\\%ls failed (0x%08lX)", storeName, GetLastError());
    CertCloseStore(store, 0);
    return ok;
}

static int RemoveFromStore(const wchar_t* storeName)
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, storeName);
    if (!store) return 0;
    int removed = 0;
    PCCERT_CONTEXT c = nullptr;
    while ((c = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, kCertName, c))) {
        wchar_t subject[256];
        CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR, subject, ARRAYSIZE(subject));
        if (_wcsicmp(subject, kCertSubject) != 0) continue;
        PCCERT_CONTEXT dup = CertDuplicateCertificateContext(c);
        if (CertDeleteCertificateFromStore(dup)) removed++;  // frees dup
    }
    CertCloseStore(store, 0);
    if (removed) Log(L"Removed %d certificate(s) from LocalMachine\\%ls", removed, storeName);
    return removed;
}

// ---------------------------------------------------------------------------
// WinUHid device and driver package (same steps as the engine's install-driver)
// ---------------------------------------------------------------------------

static int ForEachWinUHidDevice(bool remove)
{
    int count = 0;
    // Without DIGCF_PRESENT so removal also catches devices that are currently not started.
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    SP_DEVINFO_DATA d = {sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
        wchar_t ids[1024] = {};
        DEVPROPTYPE type;
        if (!SetupDiGetDevicePropertyW(set, &d, &DEVPKEY_Device_HardwareIds, &type, (PBYTE)ids, sizeof(ids), nullptr,
                                       0) ||
            memcmp(ids, kWinUHidHwIds, sizeof(kWinUHidHwIds)) != 0) {
            continue;
        }
        count++;
        if (remove && !DiUninstallDevice(nullptr, set, &d, 0, nullptr))
            Log(L"Removing a WinUHid device failed (0x%08lX)", GetLastError());
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

static bool CreateWinUHidDevice(const fs::path& inf)
{
    GUID cls;
    wchar_t clsName[MAX_CLASS_NAME_LEN];
    if (!SetupDiGetINFClassW(inf.c_str(), &cls, clsName, MAX_CLASS_NAME_LEN, nullptr))
        return Log(L"Bad INF %ls (0x%08lX)", inf.c_str(), GetLastError()), false;
    HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
    SP_DEVINFO_DATA d = {sizeof(d)};
    bool ok = SetupDiCreateDeviceInfoW(set, L"WinUHid", &cls, nullptr, nullptr, DICD_GENERATE_ID, &d) &&
              SetupDiSetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, (const BYTE*)kWinUHidHwIds,
                                                sizeof(kWinUHidHwIds)) &&
              SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d);
    if (!ok) Log(L"Creating the WinUHid device failed (0x%08lX)", GetLastError());
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

// Published names (oemNN.inf) of every WinUHid package in the driver store.
static std::vector<std::wstring> WinUHidPackages()
{
    std::vector<std::wstring> out;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(fs::path(windir) / L"INF", ec)) {
        std::wstring name = entry.path().filename().wstring();
        if (_wcsnicmp(name.c_str(), L"oem", 3) != 0 || _wcsicmp(entry.path().extension().c_str(), L".inf") != 0)
            continue;
        DWORD size = 0;
        SetupGetInfInformationW(entry.path().c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, nullptr, 0, &size);
        if (!size) continue;
        std::vector<BYTE> buf(size);
        auto info = (PSP_INF_INFORMATION)buf.data();
        if (!SetupGetInfInformationW(entry.path().c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, info, size, nullptr))
            continue;
        SP_ORIGINAL_FILE_INFO_W orig = {sizeof(orig)};
        if (SetupQueryInfOriginalFileInformationW(info, 0, nullptr, &orig) &&
            _wcsicmp(orig.OriginalInfName, kDriverInf) == 0)
            out.push_back(name);
    }
    return out;
}

static void RemoveDriverPackages()
{
    for (auto& oem : WinUHidPackages()) {
        if (SetupUninstallOEMInfW(oem.c_str(), SUOI_FORCEDELETE, nullptr))
            Log(L"Removed driver package %ls", oem.c_str());
        else
            Log(L"Removing driver package %ls failed (0x%08lX)", oem.c_str(), GetLastError());
    }
}

// ---------------------------------------------------------------------------

static bool CopyDriver(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    fs::create_directories(to, ec);
    for (const wchar_t* f : {L"WinUHidDriver.inf", L"WinUHidDriver.dll", L"winuhiddriver.cat"}) {
        fs::copy_file(from / f, to / f, fs::copy_options::overwrite_existing, ec);
        if (ec) return Log(L"Copying %ls failed: %hs", f, ec.message().c_str()), false;
    }
    return true;
}

static void RemoveEverything(bool removeSettings)
{
    int n = ForEachWinUHidDevice(true);
    Log(L"Removed %d WinUHid device(s)", n);
    RemoveDriverPackages();
    RemoveFromStore(L"Root");
    RemoveFromStore(L"TrustedPublisher");
    if (removeSettings) {
        PWSTR base = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) {
            std::error_code ec;
            fs::path dir = fs::path(base) / L"SSIM";
            fs::remove_all(dir, ec);
            Log(L"Removed %ls%hs", dir.c_str(), ec ? (" (failed: " + ec.message() + ")").c_str() : "");
        }
        CoTaskMemFree(base);
    }
}

static bool SignAndInstallDriver(const fs::path& installDir)
{
    // Start from a clean slate: an earlier install's certificate and packages.
    RemoveDriverPackages();
    RemoveFromStore(L"Root");
    RemoveFromStore(L"TrustedPublisher");

    // Sign a private copy; the files under Program Files stay as shipped.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path work = fs::path(tmp) / (L"SSIM-driver-" + std::to_wstring(GetTickCount64()));
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{work};
    if (!CopyDriver(installDir / L"driver", work)) return false;

    {
        SigningKey key;
        if (!key.Create()) return false;
        PCCERT_CONTEXT cert = CreateCertificate(key);
        if (!cert) return false;
        bool ok = SignFile(cert, work / L"WinUHidDriver.dll") && SignFile(cert, work / L"winuhiddriver.cat") &&
                  AddToStore(L"Root", cert) && AddToStore(L"TrustedPublisher", cert);
        CertFreeCertificateContext(cert);
        if (!ok) return false;
    }  // the key container is deleted here

    fs::path inf = work / kDriverInf;
    if (ForEachWinUHidDevice(false) == 0) {
        if (!CreateWinUHidDevice(inf)) return false;
        Log(L"Created the WinUHid root device");
    }
    BOOL reboot = FALSE;
    if (!DiInstallDriverW(nullptr, inf.c_str(), DIIRFLAG_FORCE_INF, &reboot))
        return Log(L"Driver install failed (0x%08lX)", GetLastError()), false;
    Log(L"WinUHid driver installed%ls", reboot ? L" (restart required)" : L"");
    if (reboot) MsiSetMode(g_msi, MSIRUNMODE_REBOOTATEND, TRUE);
    return true;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

// CustomActionData: INSTALLDIR=<dir>
extern "C" UINT __stdcall SignAndInstall(MSIHANDLE h)
{
    g_msi = h;
    std::wstring data = GetProp(L"CustomActionData");
    fs::path dir = DataValue(data, L"INSTALLDIR");
    Log(L"Installing the WinUHid driver from %ls", dir.c_str());
    return SignAndInstallDriver(dir) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

// CustomActionData: REMOVESETTINGS=<1 or empty>
extern "C" UINT __stdcall RemoveAll(MSIHANDLE h)
{
    g_msi = h;
    std::wstring data = GetProp(L"CustomActionData");
    RemoveEverything(DataValue(data, L"REMOVESETTINGS") == L"1");
    return ERROR_SUCCESS;  // never block an uninstall
}

// Runs from the UI before installation starts: HidHide is an MSI itself, and Windows can only run
// one installation at a time, so it can't be installed from inside SSIM's own install script.
// Sets SSIM_HIDHIDE to "installed", "reboot", "declined" or "failed".
extern "C" UINT __stdcall InstallHidHide(MSIHANDLE h)
{
    g_msi = h;
    auto set = [](const wchar_t* v) { MsiSetPropertyW(g_msi, L"SSIM_HIDHIDE", v); };

    // Extract the installer from the Binary table.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path exe = fs::path(tmp) / L"SSIM-HidHide_x64.exe";
    {
        PMSIHANDLE db = MsiGetActiveDatabase(h);
        PMSIHANDLE view, rec;
        if (MsiDatabaseOpenViewW(db, L"SELECT `Data` FROM `Binary` WHERE `Name`='HidHideSetup'", &view) ||
            MsiViewExecute(view, 0) || MsiViewFetch(view, &rec)) {
            Log(L"HidHide installer not found in the package");
            return set(L"failed"), ERROR_SUCCESS;
        }
        HANDLE f = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return Log(L"Cannot write %ls", exe.c_str()), set(L"failed"), ERROR_SUCCESS;
        std::vector<char> buf(1 << 16);
        for (;;) {
            DWORD n = (DWORD)buf.size(), written;
            if (MsiRecordReadStream(rec, 1, buf.data(), &n) || n == 0) break;
            WriteFile(f, buf.data(), n, &written, nullptr);
        }
        CloseHandle(f);
    }

    // HidHide's installer asks for administrator rights itself (UAC).
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = L"/exenoui /qn REBOOT=ReallySuppress";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        DeleteFileW(exe.c_str());
        Log(L"Starting the HidHide installer failed (%lu)", e);
        return set(e == ERROR_CANCELLED ? L"declined" : L"failed"), ERROR_SUCCESS;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    DeleteFileW(exe.c_str());
    Log(L"HidHide installer finished (%lu)", code);
    set(code == 0 ? L"installed" : code == ERROR_SUCCESS_REBOOT_REQUIRED ? L"reboot" : L"failed");
    return ERROR_SUCCESS;
}
