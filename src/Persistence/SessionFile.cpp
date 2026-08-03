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
    // Annotated on creation, so the file explains itself — see
    // RemoteSettings::SaveToIni for why this has to happen at creation time.
    if (!Ini::Exists(Path())) {
        std::wstring body = L"; QuickImageViewer\r\n; ";
        body += CS::FILE_HEADER;
        body += L"\r\n";
        body += Ini::GeneratedStampLines();
        body += L";\r\n"
                L"; Not a settings file. Nothing here is worth keeping: delete it and the\r\n"
                L"; next launch simply opens from the folder history instead.\r\n"
                L"; It is separate so that closing qIV does not rewrite every setting the\r\n"
                L"; application has just to record one line.\r\n\r\n";
        body += L"["; body += CS::SECTION; body += L"]\r\n\r\n";
        body += L"; Full path of the image on screen at the last exit. Reopened on the\r\n"
                L"; next launch. Ignored if the file no longer exists.\r\n";
        body += CS::KEY_LAST_IMAGE; body += L"="; body += fullPath; body += L"\r\n";
        Ini::CreateWithTextIfMissing(Path(), body);
    }

    // Written straight through rather than queued. Called from the exit path,
    // where the point is that it reaches disk before teardown.
    Ini::WriteString(Path(), CS::SECTION, CS::KEY_LAST_IMAGE, fullPath,
                     CS::FILE_HEADER);
    // Doubles as a record of when qIV last exited.
    Ini::TouchUpdatedStamp(Path());
}

} // namespace Persistence::Session
