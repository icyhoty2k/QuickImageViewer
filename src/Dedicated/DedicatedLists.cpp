#include "DedicatedLists.h"
#include "DedicatedSettings.h"
#include "Persistence/RegistryManager.h" // GetExePathW
#include "Platform/Constants.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>

namespace Dedicated {

namespace {
    // [Instance] keys recording which list files this instance uses.
    constexpr const wchar_t *KEY_IMAGE_LISTS = L"ImageLists";
    constexpr const wchar_t *KEY_PROMO_LISTS = L"PromotionLists";

    std::wstring g_imagePath;
    std::wstring g_promoPath;
    bool         g_resolved = false;

    std::wstring ExeFolder() {
        std::wstring exe = Persistence::Registry::GetExePathW();
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        exe.resize(slash + 1);
        return exe;
    }

    bool FileExists(const std::wstring &p) {
        if (p.empty()) return false;
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Creates an empty, self-describing list file. UTF-16LE with a BOM to match
    // the .ini and so folder paths with non-ASCII characters survive.
    void CreateEmptyList(const std::wstring &path, const wchar_t *what) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;

        std::wstring head;
        head += static_cast<wchar_t>(0xFEFF); // BOM first
        head += L"; QuickImageViewer - ";
        head += what;
        head += L"\r\n; One folder per line. Lines starting with # or ; are ignored.\r\n";

        DWORD written = 0;
        WriteFile(h, head.data(),
                  static_cast<DWORD>(head.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(h);
    }

    // Resolve once: read the recorded names, generate + record any that are
    // missing, then make sure both files exist.
    void Resolve() {
        if (g_resolved) return;
        g_resolved = true;
        if (!SettingsUseFile()) return; // not a dedicated instance — no lists

        namespace D = Constants::Dedicated;
        const std::wstring folder = ExeFolder();
        const std::wstring stem   = ExeStemName();
        if (folder.empty()) return;

        std::wstring imageName = ReadInstanceString(KEY_IMAGE_LISTS);
        std::wstring promoName = ReadInstanceString(KEY_PROMO_LISTS);

        if (imageName.empty()) {
            imageName = std::wstring(D::IMAGE_LIST_PREFIX) + stem + D::LIST_FILE_EXT;
            WriteInstanceString(KEY_IMAGE_LISTS, imageName);
        }
        if (promoName.empty()) {
            promoName = std::wstring(D::PROMO_LIST_PREFIX) + stem + D::LIST_FILE_EXT;
            WriteInstanceString(KEY_PROMO_LISTS, promoName);
        }

        // A recorded name may already be an absolute path — that is how two
        // instances share one list. Only bare names are resolved next to the exe.
        auto fullPath = [&](const std::wstring &name) {
            const bool absolute = name.size() > 2 &&
                                  (name[1] == L':' || (name[0] == L'\\' && name[1] == L'\\'));
            return absolute ? name : folder + name;
        };

        g_imagePath = fullPath(imageName);
        g_promoPath = fullPath(promoName);

        if (!FileExists(g_imagePath)) CreateEmptyList(g_imagePath, L"image folders");
        if (!FileExists(g_promoPath)) CreateEmptyList(g_promoPath, L"promotion folders");
    }

    std::vector<std::wstring> LoadList(const std::wstring &path) {
        std::vector<std::wstring> out;
        if (path.empty()) return out;

        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return out;

        wchar_t line[Constants::MAX_FILE_PATH];
        while (fgetws(line, static_cast<int>(std::size(line)), f)) {
            std::wstring s(line);
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();

            const size_t b = s.find_first_not_of(L" \t");
            if (b == std::wstring::npos) continue;          // blank
            if (s[b] == L'#' || s[b] == L';') continue;     // comment
            const size_t e = s.find_last_not_of(L" \t");
            out.push_back(s.substr(b, e - b + 1));
        }
        fclose(f);
        return out;
    }

    bool SaveList(const std::wstring &path, const std::vector<std::wstring> &folders,
                  const wchar_t *what) {
        if (path.empty()) return false;
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || !f) return false;

        fwprintf(f, L"; QuickImageViewer - %s\n", what);
        fwprintf(f, L"; One folder per line. Lines starting with # or ; are ignored.\n");
        for (const std::wstring &p : folders)
            if (!p.empty()) fwprintf(f, L"%s\n", p.c_str());
        fclose(f);
        return true;
    }
}

// =============================================================================
// Authoring another instance's lists
// =============================================================================
namespace {
    // <exe folder>\<prefix><exe stem>.txt
    std::wstring ListPathFor(const std::wstring &exePath, const wchar_t *prefix) {
        if (exePath.empty()) return {};
        const size_t slash = exePath.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};

        std::wstring folder = exePath.substr(0, slash + 1);
        std::wstring stem   = exePath.substr(slash + 1);
        const size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem.resize(dot);

        return folder + prefix + stem + Constants::Dedicated::LIST_FILE_EXT;
    }

    // Case-insensitive, trailing-slash-insensitive comparison key.
    std::wstring FolderKey(std::wstring p) {
        while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
        std::transform(p.begin(), p.end(), p.begin(), ::towlower);
        return p;
    }
}

std::wstring ImageListPathFor(const std::wstring &exePath) {
    return ListPathFor(exePath, Constants::Dedicated::IMAGE_LIST_PREFIX);
}

std::wstring PromotionListPathFor(const std::wstring &exePath) {
    return ListPathFor(exePath, Constants::Dedicated::PROMO_LIST_PREFIX);
}

AppendResult AppendFolder(const std::wstring &listPath, const std::wstring &folder) {
    if (listPath.empty() || folder.empty()) return AppendResult::Failed;

    if (!FileExists(listPath))
        CreateEmptyList(listPath, L"folder list");
    if (!FileExists(listPath)) return AppendResult::Failed;

    // Duplicate check against what is already listed. Skipping silently would
    // leave the user unsure whether the click registered, so the caller reports
    // it — this only decides.
    const std::wstring key = FolderKey(folder);
    for (const std::wstring &existing : LoadList(listPath))
        if (FolderKey(existing) == key) return AppendResult::Duplicate;

    // Append rather than rewrite: hand-written comments and ordering survive.
    FILE *f = nullptr;
    if (_wfopen_s(&f, listPath.c_str(), L"a, ccs=UTF-8") != 0 || !f)
        return AppendResult::Failed;
    fwprintf(f, L"%s\n", folder.c_str());
    fclose(f);
    return AppendResult::Added;
}

void EnsureListFiles() { Resolve(); }

const std::wstring &ImageListPath()     { Resolve(); return g_imagePath; }
const std::wstring &PromotionListPath() { Resolve(); return g_promoPath; }

std::vector<std::wstring> LoadImageFolders()     { Resolve(); return LoadList(g_imagePath); }
std::vector<std::wstring> LoadPromotionFolders() { Resolve(); return LoadList(g_promoPath); }

bool SaveImageFolders(const std::vector<std::wstring> &folders) {
    Resolve();
    return SaveList(g_imagePath, folders, L"image folders");
}

bool SavePromotionFolders(const std::vector<std::wstring> &folders) {
    Resolve();
    return SaveList(g_promoPath, folders, L"promotion folders");
}

} // namespace Dedicated
