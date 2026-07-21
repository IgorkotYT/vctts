#include "driver_setup.h"

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <newdev.h>
#include <propvarutil.h>
#include <propsys.h>
#include <setupapi.h>
#include <urlmon.h>
#include <wincrypt.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct IPolicyConfig : public IUnknown
{
    virtual HRESULT GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetDefaultEndpoint(PCWSTR wszDeviceId, ERole role) = 0;
    virtual HRESULT SetEndpointVisibility(PCWSTR, INT) = 0;
};

static const CLSID CLSID_PolicyConfigClient =
{ 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
static const IID IID_IPolicyConfig =
{ 0xf8679f50, 0x850a, 0x41cf, { 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8 } };

namespace
{
    constexpr const wchar_t* kDriverVersion = L"25.7.14";
    constexpr const wchar_t* kDriverZipName = L"Virtual.Audio.Driver.Signed.-.25.7.14.zip";
    constexpr const wchar_t* kDriverReleaseUrl =
        L"https://github.com/VirtualDrivers/Virtual-Audio-Driver/releases/download/25.7.14/Virtual.Audio.Driver.Signed.-.25.7.14.zip";
    constexpr const wchar_t* kDriverZipSha256 =
        L"DD10560994DE65A7E587FB8B93C0D7E9838292D9C3566A0976C2786D727292BD";
    constexpr const wchar_t* kDriverInfRelative =
        L"25.7.14\\Virtual Audio Driver\\VirtualAudioDriver.inf";
    constexpr const wchar_t* kDefaultDriverHardwareId = L"ROOT\\VirtualAudioDriver";
    constexpr const wchar_t* kManifestText =
        L"# Downloaded by vctts from VirtualDrivers/Virtual-Audio-Driver.\r\n"
        L"version=25.7.14\r\n"
        L"inf=25.7.14\\Virtual Audio Driver\\VirtualAudioDriver.inf\r\n"
        L"hardware_id=ROOT\\VirtualAudioDriver\r\n"
        L"playback_matches=Virtual Audio Driver by MTT|Virtual Audio|Virtual Speaker|Cable Input|Voicemeeter Input\r\n"
        L"capture_matches=Virtual Mic Driver by MTT|Virtual Mic|Virtual Microphone|Cable Output|Voicemeeter Output\r\n";

    std::wstring trim(std::wstring s)
    {
        auto is_ws = [](wchar_t c) { return std::iswspace(c) != 0; };
        while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
        while (!s.empty() && is_ws(s.back())) s.pop_back();
        return s;
    }

    std::wstring lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
            return (wchar_t)std::towlower(c);
        });
        return s;
    }

    std::vector<std::wstring> split_list(const std::wstring& value)
    {
        std::vector<std::wstring> out;
        std::wstringstream ss(value);
        std::wstring item;
        while (std::getline(ss, item, L'|')) {
            item = trim(item);
            if (!item.empty())
                out.push_back(item);
        }
        return out;
    }

    std::filesystem::path executable_path()
    {
        wchar_t path[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
        if (length == 0 || length >= std::size(path))
            return {};
        return std::filesystem::path(path);
    }

    std::filesystem::path app_dir()
    {
        return executable_path().parent_path();
    }

    std::filesystem::path manifest_path()
    {
        return app_dir() / L"drivers" / L"VirtualAudioDriver" / L"manifest.txt";
    }

    std::filesystem::path driver_root()
    {
        return manifest_path().parent_path();
    }

    std::filesystem::path bundled_inf_path()
    {
        return driver_root() / kDriverInfRelative;
    }

    std::filesystem::path zip_path()
    {
        return driver_root() / kDriverZipName;
    }

    std::filesystem::path resolve_inf_path(const driver_setup::Manifest& manifest)
    {
        std::filesystem::path inf(manifest.infPath);
        if (inf.is_absolute())
            return inf;
        return manifest_path().parent_path() / inf;
    }

    bool write_default_manifest()
    {
        std::filesystem::create_directories(driver_root());
        std::wofstream file(manifest_path(), std::ios::trunc);
        if (!file)
            return false;
        file << kManifestText;
        return true;
    }

    bool package_complete()
    {
        driver_setup::Manifest manifest;
        if (driver_setup::load_manifest(manifest) && std::filesystem::exists(resolve_inf_path(manifest)))
            return true;

        if (std::filesystem::exists(bundled_inf_path())) {
            write_default_manifest();
            return true;
        }
        return false;
    }

    bool run_hidden_and_wait(const std::wstring& exe, const std::wstring& args)
    {
        std::wstring command = L"\"" + exe + L"\" " + args;
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi))
            return false;

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return code == 0;
    }

    bool hardware_id_list_contains(const wchar_t* ids, size_t charCount, const std::wstring& wanted)
    {
        if (!ids || wanted.empty())
            return false;

        size_t offset = 0;
        while (offset < charCount && ids[offset] != L'\0') {
            size_t remaining = charCount - offset;
            size_t length = wcsnlen_s(ids + offset, remaining);
            if (length == remaining)
                return false;
            if (_wcsicmp(ids + offset, wanted.c_str()) == 0)
                return true;
            offset += length + 1;
        }
        return false;
    }

    DWORD find_existing_device(const GUID& classGuid, const std::wstring& hardwareId, bool& found)
    {
        found = false;
        HDEVINFO devices = SetupDiGetClassDevsW(&classGuid, nullptr, nullptr, 0);
        if (devices == INVALID_HANDLE_VALUE)
            return GetLastError();

        DWORD result = ERROR_SUCCESS;
        for (DWORD index = 0; ; ++index) {
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
                DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_ITEMS)
                    result = error;
                break;
            }

            DWORD type = 0;
            DWORD required = 0;
            SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID,
                                              &type, nullptr, 0, &required);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0)
                continue;

            std::vector<BYTE> value(required + sizeof(wchar_t), 0);
            if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID,
                                                   &type, value.data(), required, nullptr))
                continue;
            if (type != REG_SZ && type != REG_MULTI_SZ)
                continue;

            if (hardware_id_list_contains(reinterpret_cast<const wchar_t*>(value.data()),
                                          value.size() / sizeof(wchar_t), hardwareId)) {
                found = true;
                break;
            }
        }

        SetupDiDestroyDeviceInfoList(devices);
        return result;
    }

    DWORD create_root_device(const GUID& classGuid,
                             const wchar_t* className,
                             const std::wstring& hardwareId,
                             HDEVINFO& deviceSet,
                             SP_DEVINFO_DATA& device)
    {
        deviceSet = SetupDiCreateDeviceInfoList(&classGuid, nullptr);
        if (deviceSet == INVALID_HANDLE_VALUE)
            return GetLastError();

        device = {};
        device.cbSize = sizeof(device);
        if (!SetupDiCreateDeviceInfoW(deviceSet, className, &classGuid, nullptr, nullptr,
                                      DICD_GENERATE_ID, &device)) {
            DWORD error = GetLastError();
            SetupDiDestroyDeviceInfoList(deviceSet);
            deviceSet = INVALID_HANDLE_VALUE;
            return error;
        }

        std::vector<wchar_t> hardwareIds(hardwareId.begin(), hardwareId.end());
        hardwareIds.push_back(L'\0');
        hardwareIds.push_back(L'\0');
        const DWORD hardwareIdsBytes = static_cast<DWORD>(hardwareIds.size() * sizeof(wchar_t));
        if (!SetupDiSetDeviceRegistryPropertyW(deviceSet, &device, SPDRP_HARDWAREID,
                                               reinterpret_cast<const BYTE*>(hardwareIds.data()),
                                               hardwareIdsBytes)) {
            DWORD error = GetLastError();
            SetupDiDestroyDeviceInfoList(deviceSet);
            deviceSet = INVALID_HANDLE_VALUE;
            return error;
        }

        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, deviceSet, &device)) {
            DWORD error = GetLastError();
            SetupDiDestroyDeviceInfoList(deviceSet);
            deviceSet = INVALID_HANDLE_VALUE;
            return error;
        }

        return ERROR_SUCCESS;
    }

    std::wstring bytes_to_hex(const BYTE* bytes, DWORD count)
    {
        static constexpr wchar_t hex[] = L"0123456789ABCDEF";
        std::wstring out;
        out.reserve((size_t)count * 2);
        for (DWORD i = 0; i < count; ++i) {
            out.push_back(hex[(bytes[i] >> 4) & 0xF]);
            out.push_back(hex[bytes[i] & 0xF]);
        }
        return out;
    }

    bool sha256_matches(const std::filesystem::path& path, const std::wstring& expected)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        HCRYPTPROV provider = 0;
        HCRYPTHASH hash = 0;
        bool ok = false;

        if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
            CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
            BYTE buffer[8192];
            DWORD read = 0;
            BOOL readOk = TRUE;
            while ((readOk = ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) && read > 0) {
                if (!CryptHashData(hash, buffer, read, 0)) {
                    readOk = FALSE;
                    break;
                }
            }

            if (readOk) {
                BYTE digest[32];
                DWORD digestSize = sizeof(digest);
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0))
                    ok = bytes_to_hex(digest, digestSize) == expected;
            }
        }

        if (hash) CryptDestroyHash(hash);
        if (provider) CryptReleaseContext(provider, 0);
        CloseHandle(file);
        return ok;
    }

    bool extract_zip()
    {
        std::filesystem::create_directories(driver_root() / kDriverVersion);
        std::wstring args =
            L"-NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
            zip_path().wstring() + L"' -DestinationPath '" +
            (driver_root() / kDriverVersion).wstring() + L"' -Force\"";
        return run_hidden_and_wait(L"powershell.exe", args);
    }

    std::wstring endpoint_name(IMMDevice* device)
    {
        if (!device) return {};
        ComPtr<IPropertyStore> props;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props)
            return {};

        PROPVARIANT value;
        PropVariantInit(&value);
        std::wstring name;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            name = value.pwszVal;
        PropVariantClear(&value);
        return name;
    }
}

namespace driver_setup
{
    bool parse_manifest_text(const std::wstring& text, Manifest& manifest)
    {
        manifest = Manifest{};
        std::wstringstream ss(text);
        std::wstring line;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (line.empty() || line[0] == L'#')
                continue;

            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos)
                continue;

            std::wstring key = lower(trim(line.substr(0, eq)));
            std::wstring value = trim(line.substr(eq + 1));
            if (key == L"version") manifest.version = value;
            else if (key == L"inf") manifest.infPath = value;
            else if (key == L"hardware_id") manifest.hardwareId = value;
            else if (key == L"playback_matches") manifest.playbackMatches = split_list(value);
            else if (key == L"capture_matches") manifest.captureMatches = split_list(value);
        }

        return !manifest.infPath.empty() &&
               !manifest.playbackMatches.empty() &&
               !manifest.captureMatches.empty();
    }

    bool load_manifest(Manifest& manifest)
    {
        std::wifstream file(manifest_path());
        if (!file)
            return false;
        std::wstringstream buffer;
        buffer << file.rdbuf();
        return parse_manifest_text(buffer.str(), manifest);
    }

    int find_matching_device(const std::vector<AudioDevice>& devices, const std::vector<std::wstring>& matches)
    {
        // Prefer the manifest's match order and exact endpoint names. Otherwise an
        // unrelated cable listed first by Windows can beat the bundled MTT driver.
        for (const std::wstring& match : matches) {
            std::wstring needle = lower(match);
            if (needle.empty())
                continue;
            for (int i = 0; i < (int)devices.size(); ++i) {
                if (lower(devices[i].name) == needle)
                    return i;
            }
        }

        for (const std::wstring& match : matches) {
            std::wstring needle = lower(match);
            if (needle.empty())
                continue;
            for (int i = 0; i < (int)devices.size(); ++i) {
                if (lower(devices[i].name).find(needle) != std::wstring::npos)
                    return i;
            }
        }
        return -1;
    }

    DriverSetupStatus refresh_status(AppState& state)
    {
        Manifest manifest;
        DriverSetupStatus status;
        status.manifestFound = load_manifest(manifest);
        status.driverFolderFound = std::filesystem::exists(manifest_path().parent_path());

        std::vector<std::wstring> playbackMatches = status.manifestFound
            ? manifest.playbackMatches
            : std::vector<std::wstring>{ L"virtual audio", L"virtual speaker", L"cable input", L"voicemeeter input" };
        std::vector<std::wstring> captureMatches = status.manifestFound
            ? manifest.captureMatches
            : std::vector<std::wstring>{ L"virtual audio", L"virtual microphone", L"cable output", L"voicemeeter output" };

        int out = find_matching_device(state.outDevices, playbackMatches);
        int in = find_matching_device(state.inDevices, captureMatches);
        status.virtualPlaybackFound = out >= 0;
        status.virtualCaptureFound = in >= 0;
        if (out >= 0) state.bridgeVirtualOutDev = out;
        if (in >= 0) state.bridgeVirtualInDev = in;

        if (!status.manifestFound)
            status.message = L"Driver manifest missing; using manual virtual-device detection.";
        else if (!status.virtualPlaybackFound || !status.virtualCaptureFound)
            status.message = L"Driver package found, but virtual endpoints are not active.";
        else
            status.message = L"Virtual audio endpoints detected.";

        state.driverSetup = status;
        return status;
    }

    bool ensure_driver_package(HWND owner)
    {
        if (package_complete())
            return true;

        int download = MessageBoxW(
            owner,
            L"The bundled virtual audio driver package is missing or incomplete.\n\n"
            L"Download the signed Virtual Audio Driver 25.7.14 release from GitHub now?",
            L"Mic Bridge Driver",
            MB_YESNO | MB_ICONQUESTION);
        if (download != IDYES)
            return false;

        std::filesystem::create_directories(driver_root());

        HRESULT hr = URLDownloadToFileW(nullptr, kDriverReleaseUrl, zip_path().c_str(), 0, nullptr);
        if (FAILED(hr)) {
            MessageBoxW(owner,
                        L"Driver download failed. Check your internet connection and try again.",
                        L"Mic Bridge Driver",
                        MB_OK | MB_ICONERROR);
            return false;
        }

        if (!sha256_matches(zip_path(), kDriverZipSha256)) {
            DeleteFileW(zip_path().c_str());
            MessageBoxW(owner,
                        L"Driver download hash verification failed. The downloaded file was deleted.",
                        L"Mic Bridge Driver",
                        MB_OK | MB_ICONERROR);
            return false;
        }

        if (!extract_zip() || !std::filesystem::exists(bundled_inf_path())) {
            MessageBoxW(owner,
                        L"Driver ZIP downloaded, but extraction did not produce the expected INF file.",
                        L"Mic Bridge Driver",
                        MB_OK | MB_ICONERROR);
            return false;
        }

        if (!write_default_manifest()) {
            MessageBoxW(owner,
                        L"The driver was extracted, but its manifest could not be written.",
                        L"Mic Bridge Driver",
                        MB_OK | MB_ICONERROR);
            return false;
        }

        int install = MessageBoxW(
            owner,
            L"Driver package downloaded and prepared.\n\nLaunch the administrator installer now?",
            L"Mic Bridge Driver",
            MB_YESNO | MB_ICONQUESTION);
        return install == IDYES;
    }

    bool install_or_repair(HWND owner)
    {
        if (!ensure_driver_package(owner))
            return false;

        Manifest manifest;
        if (!load_manifest(manifest))
            return false;

        std::filesystem::path inf = resolve_inf_path(manifest);
        if (!std::filesystem::exists(inf))
            return false;

        std::filesystem::path exe = executable_path();
        if (exe.empty())
            return false;

        std::wstring exeString = exe.wstring();
        std::wstring directoryString = app_dir().wstring();
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = owner;
        sei.lpVerb = L"runas";
        sei.lpFile = exeString.c_str();
        sei.lpParameters = kElevatedInstallArgument;
        sei.lpDirectory = directoryString.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {
            DWORD error = GetLastError();
            if (error != ERROR_CANCELLED) {
                std::wstring message = L"Could not start the administrator installer (Windows error " +
                                       std::to_wstring(error) + L").";
                MessageBoxW(owner, message.c_str(), L"Mic Bridge Driver", MB_OK | MB_ICONERROR);
            }
            return false;
        }

        if (WaitForSingleObject(sei.hProcess, INFINITE) != WAIT_OBJECT_0) {
            CloseHandle(sei.hProcess);
            return false;
        }

        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);

        if (code == ERROR_SUCCESS_REBOOT_REQUIRED) {
            MessageBoxW(owner,
                        L"The virtual audio driver was installed. Restart Windows to finish setup.",
                        L"Mic Bridge Driver",
                        MB_OK | MB_ICONINFORMATION);
        } else if (!is_successful_install_exit_code(code)) {
            std::wstring message = L"Windows could not install the virtual audio device (error " +
                                   std::to_wstring(code) +
                                   L").\n\nDetails are recorded in C:\\Windows\\INF\\setupapi.dev.log.";
            MessageBoxW(owner, message.c_str(), L"Mic Bridge Driver", MB_OK | MB_ICONERROR);
        }
        return is_successful_install_exit_code(code);
    }

    bool is_successful_install_exit_code(DWORD code)
    {
        return code == ERROR_SUCCESS ||
               code == ERROR_SUCCESS_REBOOT_REQUIRED ||
               code == ERROR_SUCCESS_REBOOT_INITIATED;
    }

    DWORD install_driver_elevated()
    {
        Manifest manifest;
        if (!load_manifest(manifest))
            return ERROR_FILE_NOT_FOUND;

        const std::filesystem::path inf = resolve_inf_path(manifest);
        std::error_code fileError;
        if (!std::filesystem::is_regular_file(inf, fileError))
            return fileError ? static_cast<DWORD>(fileError.value()) : ERROR_FILE_NOT_FOUND;

        const std::wstring hardwareId = manifest.hardwareId.empty()
            ? std::wstring(kDefaultDriverHardwareId)
            : manifest.hardwareId;

        GUID classGuid{};
        wchar_t className[256]{};
        if (!SetupDiGetINFClassW(inf.c_str(), &classGuid, className,
                                 static_cast<DWORD>(std::size(className)), nullptr))
            return GetLastError();

        bool deviceExists = false;
        DWORD result = find_existing_device(classGuid, hardwareId, deviceExists);
        if (result != ERROR_SUCCESS)
            return result;

        HDEVINFO createdDeviceSet = INVALID_HANDLE_VALUE;
        SP_DEVINFO_DATA createdDevice{};
        if (!deviceExists) {
            result = create_root_device(classGuid, className, hardwareId,
                                        createdDeviceSet, createdDevice);
            if (result != ERROR_SUCCESS)
                return result;
        }

        BOOL rebootRequired = FALSE;
        if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hardwareId.c_str(), inf.c_str(),
                                                INSTALLFLAG_FORCE, &rebootRequired)) {
            result = GetLastError();
            if (createdDeviceSet != INVALID_HANDLE_VALUE)
                SetupDiCallClassInstaller(DIF_REMOVE, createdDeviceSet, &createdDevice);
        }

        if (createdDeviceSet != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(createdDeviceSet);

        if (result != ERROR_SUCCESS)
            return result;
        return rebootRequired ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
    }

    bool set_default_communications_capture(const AppState& state, HWND owner)
    {
        if (state.bridgeVirtualInDev < 0 || state.bridgeVirtualInDev >= (int)state.inDevices.size())
            return false;

        HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool didInit = SUCCEEDED(initHr);
        if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE)
            return false;

        bool ok = false;
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) && enumerator) {
                ComPtr<IMMDeviceCollection> devices;
                if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices)) && devices) {
                    UINT count = 0;
                    devices->GetCount(&count);
                    std::wstring targetName = lower(state.inDevices[state.bridgeVirtualInDev].name);
                    for (UINT i = 0; i < count && !ok; ++i) {
                        ComPtr<IMMDevice> device;
                        if (FAILED(devices->Item(i, &device)) || !device)
                            continue;

                        if (lower(endpoint_name(device.Get())) != targetName)
                            continue;

                        LPWSTR id = nullptr;
                        if (FAILED(device->GetId(&id)) || !id)
                            continue;

                        ComPtr<IPolicyConfig> policy;
                        if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                                                       IID_IPolicyConfig, (void**)&policy)) && policy) {
                            ok = SUCCEEDED(policy->SetDefaultEndpoint(id, eCommunications));
                        }
                        CoTaskMemFree(id);
                    }
                }
            }
        }

        if (didInit)
            CoUninitialize();

        MessageBoxW(owner,
                    ok ? L"Communications microphone updated." :
                         L"Could not set the communications microphone automatically. Select the virtual microphone in Discord/game.",
                    L"Mic Bridge",
                    ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
        return ok;
    }
}
