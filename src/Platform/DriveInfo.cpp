#include "DriveInfo.h"
#include <winioctl.h>  // IOCTL_STORAGE_QUERY_PROPERTY, StorageDeviceSeekPenaltyProperty

namespace DriveInfo {
    // ---------------------------------------------------------------------------
    // GetVolumeRootFromPath
    // ---------------------------------------------------------------------------
    // Extracts the volume root (e.g. "C:\") from any absolute path so we can
    // open the volume device for the ioctl query.
    // ---------------------------------------------------------------------------
    static std::wstring GetVolumeRootFromPath(const std::wstring &path) {
        wchar_t volumeRoot[MAX_PATH] = {};
        if (!GetVolumePathNameW(path.c_str(), volumeRoot, MAX_PATH))
            return {};
        return volumeRoot;
    }

    // ---------------------------------------------------------------------------
    // GetPhysicalDrivePath
    // ---------------------------------------------------------------------------
    // Converts a volume root like "C:\" into a physical drive path like
    // "\\.\PhysicalDrive0" via IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS.
    //
    // We need the physical drive (not the volume) because the seek-penalty ioctl
    // lives on the storage device, not the filesystem volume.
    // ---------------------------------------------------------------------------
    static std::wstring GetPhysicalDrivePath(const std::wstring &volumeRoot) {
        // Strip trailing backslash for CreateFile: "C:\" → "\\.\C:"
        std::wstring volumePath = L"\\\\.\\" + volumeRoot;
        if (!volumePath.empty() && volumePath.back() == L'\\')
            volumePath.pop_back();

        HANDLE hVol = CreateFileW(
                volumePath.c_str(),
                0, // no read/write needed
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING,
                nullptr
                );
        if (hVol == INVALID_HANDLE_VALUE)
            return {};

        // Buffer large enough for a single disk extent (most common case)
        struct {
            VOLUME_DISK_EXTENTS base;
            DISK_EXTENT extra[3]; // room for simple RAID/spanned volumes
        } extentBuf{};

        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
                hVol,
                IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                nullptr, 0,
                &extentBuf, sizeof(extentBuf),
                &bytesReturned,
                nullptr
                );
        CloseHandle(hVol);

        if (!ok || extentBuf.base.NumberOfDiskExtents < 1)
            return {};

        // Use the first disk extent — good enough for single-disk and most RAID
        DWORD diskNumber = extentBuf.base.Extents[0].DiskNumber;
        return L"\\\\.\\PhysicalDrive" + std::to_wstring(diskNumber);
    }

    // ---------------------------------------------------------------------------
    // HasSeekPenalty
    // ---------------------------------------------------------------------------
    // Opens the physical drive and queries StorageDeviceSeekPenaltyProperty.
    // Returns true  → spinning HDD  (IncursSeekPenalty == TRUE)
    // Returns false → SSD / NVMe   (IncursSeekPenalty == FALSE)
    // Returns true  → on any error (safe default: treat as HDD, use 1 thread)
    // ---------------------------------------------------------------------------
    static bool HasSeekPenalty(const std::wstring &physicalDrivePath) {
        HANDLE hDrive = CreateFileW(
                physicalDrivePath.c_str(),
                0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING,
                nullptr
                );
        if (hDrive == INVALID_HANDLE_VALUE)
            return true; // safe default

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;

        DEVICE_SEEK_PENALTY_DESCRIPTOR spd{};
        DWORD bytesReturned = 0;

        BOOL ok = DeviceIoControl(
                hDrive,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &query, sizeof(query),
                &spd, sizeof(spd),
                &bytesReturned,
                nullptr
                );
        CloseHandle(hDrive);

        if (!ok || bytesReturned < sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR))
            return true; // safe default

        return spd.IncursSeekPenalty != FALSE;
    }

    // ---------------------------------------------------------------------------
    // GetOptimalIoThreadCount  (public)
    // ---------------------------------------------------------------------------
    size_t GetOptimalIoThreadCount(const std::wstring &path) {
        std::wstring volumeRoot = GetVolumeRootFromPath(path);
        if (volumeRoot.empty())
            return 1;

        std::wstring physDrive = GetPhysicalDrivePath(volumeRoot);
        if (physDrive.empty())
            return 1;

        bool hdd = HasSeekPenalty(physDrive);

#ifdef _DEBUG
        if (hdd) {
            OutputDebugStringW((L"DriveInfo.cpp: HDD detected for " + physDrive + L" → 1 IO thread\n").c_str());
        } else {
            OutputDebugStringW((L"DriveInfo.cpp: SSD/NVMe detected for " + physDrive + L" → 2 IO threads\n").c_str());
        }
#endif

        return hdd ? 1 : 2;
    }
} // namespace DriveInfo.cpp
