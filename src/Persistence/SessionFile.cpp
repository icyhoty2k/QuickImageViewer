#include "SessionFile.h"

#include "IniFile.h"
#include "Platform/Constants.h"

namespace Persistence::Session {

namespace {
    namespace CS = Constants::Session;

    // Resolved once — the exe cannot move while the process runs.
    const std::wstring &Path() {
        static const std::wstring path = Ini::PathBesideExe(CS::FILE_NAME);
        return path;
    }
}

std::wstring LoadLastImage() {
    return Ini::ReadString(Path(), CS::SECTION, CS::KEY_LAST_IMAGE);
}

void SaveLastImage(const std::wstring &fullPath) {
    // Written straight through rather than queued. This is called from the exit
    // path, where the point is that it reaches disk before teardown, and one
    // small file is cheap enough that deferring it would buy nothing.
    Ini::WriteString(Path(), CS::SECTION, CS::KEY_LAST_IMAGE, fullPath,
                     CS::FILE_HEADER);
}

} // namespace Persistence::Session
