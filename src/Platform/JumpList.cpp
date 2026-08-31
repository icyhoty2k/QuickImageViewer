// SPDX-License-Identifier: AGPL-3.0-or-later
#include "JumpList.h"

#include <windows.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Constants.h"
#include "UI/FloatingPanels/HistoryListWnd.h" // SnapshotHistoryForRemote

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace Platform::JumpList {

    namespace {

        // This exe's own path. The shell link points at it, and the icon comes
        // from it, so both are the running binary rather than a remembered one -
        // a portable app that was moved must not offer entries that launch the
        // copy it used to be.
        std::wstring ExePath() {
            wchar_t buf[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return {};
            return buf;
        }

        // One entry: launch this exe with the folder as its argument.
        //
        // The folder is passed as a QUOTED positional argument, which is the
        // same route Explorer's "Open with" takes, so a Jump List click and a
        // double-click land in identical code. No switch is used: a switch would
        // be a second way in that could drift from the ordinary one.
        ComPtr<IShellLinkW> MakeLink(const std::wstring &exe, const std::wstring &folder,
                                     bool favourite) {
            ComPtr<IShellLinkW> link;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&link))))
                return nullptr;

            if (FAILED(link->SetPath(exe.c_str()))) return nullptr;
            const std::wstring args = L"\"" + folder + L"\"";
            if (FAILED(link->SetArguments(args.c_str()))) return nullptr;
            link->SetIconLocation(exe.c_str(), 0);

            // The TITLE is what the shell draws, and it lives in the property
            // store rather than on the link - a link's own description is the
            // tooltip. Without PKEY_Title the row comes out blank.
            //
            // The folder's own name, not the full path: the shell gives a Jump
            // List row about as much room as a menu item, and a long path is
            // ellipsised from the RIGHT, which hides the only part that
            // identifies it. The full path stays as the tooltip.
            ComPtr<IPropertyStore> store;
            if (SUCCEEDED(link.As(&store))) {
                std::wstring title = fs::path(folder).filename().wstring();
                if (title.empty()) title = folder;          // a drive root has no filename
                if (favourite) title = L"\u2605 " + title;  // the panel's own star

                PROPVARIANT pv;
                if (SUCCEEDED(InitPropVariantFromString(title.c_str(), &pv))) {
                    store->SetValue(PKEY_Title, pv);
                    store->Commit();
                    PropVariantClear(&pv);
                }
            }

            link->SetDescription(folder.c_str()); // tooltip: the whole path
            return link;
        }

    } // namespace

    void Refresh() {
        const std::wstring exe = ExePath();
        if (exe.empty()) return;

        ComPtr<ICustomDestinationList> list;
        if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&list))))
            return; // Jump Lists unavailable - nothing to report, nothing to do

        // BeginList hands back the slots the shell has room for, and the items
        // the USER removed from a previous list. Both are ignored deliberately:
        // the count because AppendCategory is free to be given more than fits and
        // the shell trims, and the removals because every entry here is
        // regenerated from the app's own history - a folder the user removed from
        // the taskbar has not been removed from qIV, and pretending otherwise
        // would make the two lists disagree.
        UINT slots = 0;
        ComPtr<IObjectArray> removed;
        if (FAILED(list->BeginList(&slots, IID_PPV_ARGS(&removed)))) return;

        ComPtr<IObjectCollection> items;
        if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&items)))) {
            list->AbortList();
            return;
        }

        // Favourites first, then recent - which is exactly the order
        // SnapshotHistoryForRemote already returns, so the taskbar, the panel and
        // the phone cannot disagree about what comes first.
        int added = 0;
        for (const auto &[folder, favourite] : UI::SnapshotHistoryForRemote()) {
            if (added >= Constants::History::JUMP_LIST_MAX) break;
            if (ComPtr<IShellLinkW> link = MakeLink(exe, folder, favourite)) {
                if (SUCCEEDED(items->AddObject(link.Get()))) ++added;
            }
        }

        if (added == 0) {
            // An empty category is worse than none: the shell draws the heading
            // with nothing under it.
            list->AbortList();
            return;
        }

        ComPtr<IObjectArray> array;
        if (FAILED(items.As(&array))) {
            list->AbortList();
            return;
        }

        if (FAILED(list->AppendCategory(Constants::History::JUMP_LIST_TITLE, array.Get()))) {
            list->AbortList();
            return;
        }

        list->CommitList();
    }

    void Clear() {
        ComPtr<ICustomDestinationList> list;
        if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&list))))
            return;

        // DeleteList(nullptr) is "this application's list", which is what the
        // shell wants when the app has no AppUserModelID of its own.
        list->DeleteList(nullptr);
    }

} // namespace Platform::JumpList
