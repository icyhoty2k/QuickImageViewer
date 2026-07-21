#pragma once
#include <string>
#include <vector>

// =============================================================================
// DedicatedLists — the two folder lists a dedicated instance runs from.
//
// A dedicated instance has NO history and NO favorites. Those exist so a person
// can find their way back to somewhere they browsed; an appliance bolted to a
// wall never browses. Keeping them would also mean N instances writing one
// shared history file and clobbering each other.
//
// In their place sit two plain text files beside the exe, one folder per line:
//
//     imageLists_<exeName>.txt      folders holding the images to show
//     promotionList_<exeName>.txt   folders holding the promotions
//
// Their real names live in the .ini under [Instance]:
//
//     ImageLists=imageLists_LobbyScreen.txt
//     PromotionLists=promotionList_LobbyScreen.txt
//
// so they can be renamed, or deliberately SHARED between instances (point two
// screens at one promotions list and both pick up the same campaign). When a
// name is absent from the .ini it is generated from the exe name; when the file
// itself is missing it is created empty. Startup therefore always ends with
// both entries recorded and both files present.
//
// Format: one folder path per line. Blank lines, and lines beginning with # or
// ; are ignored, so the files can be commented.
// =============================================================================

namespace Dedicated {

// Resolves both list names from the .ini (generating and recording them when
// absent) and creates any missing file. Safe to call on every startup.
// No-op when this process is not a dedicated instance.
void EnsureListFiles();

// Full paths of the two lists. Empty when not a dedicated instance.
const std::wstring &ImageListPath();
const std::wstring &PromotionListPath();

// Folder entries, in file order, with blanks/comments stripped. Non-existent
// folders are returned as-is — validation belongs to the caller, which can
// report a missing folder rather than silently showing nothing.
std::vector<std::wstring> LoadImageFolders();
std::vector<std::wstring> LoadPromotionFolders();

// Rewrites a list file. Used by the F8 panel when folders are added or removed.
bool SaveImageFolders(const std::vector<std::wstring> &folders);
bool SavePromotionFolders(const std::vector<std::wstring> &folders);

// --- Authoring another instance's lists -------------------------------------
// The functions above act on THIS process's lists. When the panel is preparing
// a copy that does not exist yet, the lists belong next to THAT exe, so these
// derive the paths from an arbitrary executable path.
std::wstring ImageListPathFor(const std::wstring &exePath);
std::wstring PromotionListPathFor(const std::wstring &exePath);

enum class AppendResult {
    Added,      // appended
    Duplicate,  // already listed — skipped, nothing written
    Failed,     // the file could not be created or written
};

// Appends one folder to a list file, creating the file if absent.
// Comparison is case-insensitive and ignores a trailing slash, so the same
// folder typed two ways is still recognised as a duplicate.
AppendResult AppendFolder(const std::wstring &listPath, const std::wstring &folder);

} // namespace Dedicated
