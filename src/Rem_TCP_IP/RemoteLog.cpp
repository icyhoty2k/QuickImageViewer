#include "RemoteLog.h"

// windows.h FIRST. Constants.h uses COLORREF, BYTE, DWORD, UINT and WM_USER at
// namespace scope without including it itself — every other translation unit
// reaches it through a header that already pulled windows.h in.
#include <windows.h>

#include "Platform/Constants.h"   // REMOTE_LOG_DEFAULT — the one place off is decided

#include <algorithm>
#include <atomic>
#include <cstdio>   // swprintf_s
#include <cstdlib>  // _wcstoi64
#include <deque>
#include <mutex>
#include <string>

namespace Remote::Log {

namespace {

    std::mutex        g_mutex;
    std::deque<Entry> g_entries;
    long long         g_nextSeq = 1;
    std::wstring      g_selfName;   // guarded by g_mutex — see SetSelfName

    // OUTSIDE the mutex on purpose: the whole value of the switch is that a
    // viewer with logging off never touches the lock. Relaxed ordering, because
    // nothing is published through this flag — it gates a diagnostic, and an
    // exchange landing on either side of the flip is equally true.
    std::atomic<bool> g_enabled{Constants::RemoteTcpIp::REMOTE_LOG_DEFAULT};

    // Also outside the mutex: Add must be able to decide whether to post without
    // serialising against a Snapshot the panel is taking on the UI thread.
    std::atomic<HWND> g_notifyWnd{nullptr};
    std::atomic<bool> g_notifyPending{false};

    // The file's own version marker. A loader that meets a header it does not
    // know refuses rather than guessing at the column order — a log read back
    // with the sender and receiver swapped is worse than no log.
    constexpr const wchar_t *FILE_HEADER =
        L"#qIV-remote-log\t1\tseq\twhenFt\tdeltaUs\tdir\tsender\tcommand\treceiver\tresponse";

    // Tabs and newlines are the record separators, so a field carrying one would
    // split into a row that never happened. Replaced rather than escaped: this
    // is a log to read, and an escape sequence in a response column is harder to
    // read than the space it stands in for.
    std::wstring Flatten(const std::wstring &s) {
        std::wstring out = s;
        for (wchar_t &c : out)
            if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
        return out;
    }

    // --- UTF-8, without pulling in a codec ------------------------------------
    std::string ToUtf8(const std::wstring &w) {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                          nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring FromUtf8(const char *p, size_t len) {
        if (!p || len == 0) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(len), nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(len), out.data(), n);
        return out;
    }

    // Splits on tabs WITHOUT collapsing empties: an empty response is a real
    // value (a command that answered with a bare OK), and dropping it would
    // shift every column after it.
    std::vector<std::wstring> SplitTabs(const std::wstring &line) {
        std::vector<std::wstring> out;
        size_t start = 0;
        for (;;) {
            const size_t tab = line.find(L'\t', start);
            if (tab == std::wstring::npos) { out.push_back(line.substr(start)); break; }
            out.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        return out;
    }

    long long ToLL(const std::wstring &s) {
        return _wcstoi64(s.c_str(), nullptr, 10);
    }

    std::wstring LastErrorSentence(const wchar_t *what, const std::wstring &path) {
        const DWORD e = GetLastError();
        wchar_t buf[64];
        swprintf_s(buf, L" (error %lu)", e);
        return std::wstring(what) + L":\r\n\r\n    " + path + buf;
    }

} // namespace

// =============================================================================
// Store
// =============================================================================
void Add(Direction dir,
         const std::wstring &sender,
         const std::wstring &command,
         const std::wstring &receiver,
         const std::wstring &response,
         long long deltaUs) {
    // The second half of the switch. Callers check IsEnabled() first so they
    // build nothing; this catches the ones that did not, and the race between
    // the two checks is harmless — one extra entry either side of a flip.
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);

    // TRUNCATED HERE, at the one point every producer passes through.
    //
    // An image stream puts ~128 KB of base64 on a single line, several lines per
    // picture. That is not a record of anything a reader can use, and kept whole
    // it would bloat both the in-memory store and the file it saves to. A
    // diagnostic log wants to know a chunk went past and how big it was.
    //
    // Applied to the two peer-controlled fields only; sender and receiver are this
    // program's own labels.
    // SANITISED as well as truncated, and for a reason that is not cosmetic.
    //
    // What arrives on this socket is not always text. A client that speaks TLS
    // to a plaintext listener sends a ClientHello, and those bytes land here as
    // a "command" full of control characters — including TAB and newline, which
    // are the FIELD and RECORD separators of the saved .tsv. One such line
    // silently corrupts the file's structure, so the log of the very failure you
    // are trying to diagnose is the log that cannot be read.
    //
    // Replaced rather than escaped: this is a diagnostic, and knowing a byte was
    // unprintable is all a reader can use. The count still comes from the
    // ORIGINAL length, so a clipped line reports what really went past.
    auto clip = [](const std::wstring &s) {
        namespace RT = Constants::RemoteTcpIp;

        std::wstring out;
        out.reserve(std::min(s.size(), RT::LOG_LINE_MAX));
        for (size_t i = 0; i < s.size() && i < RT::LOG_LINE_MAX; ++i) {
            const wchar_t c = s[i];
            // Everything below space, plus DEL — tab and newline among them.
            out += (c < 0x20 || c == 0x7F) ? L'·' : c;
        }

        if (s.size() > RT::LOG_LINE_MAX)
            out += L"… (" + std::to_wstring(s.size()) + L" chars)";
        return out;
    };

    Entry e;
    e.whenFt   = (static_cast<long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    e.deltaUs  = deltaUs;
    e.dir      = dir;
    e.sender   = sender;
    e.command  = clip(command);
    e.receiver = receiver;
    e.response = clip(response);

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        e.seq = g_nextSeq++;
        g_entries.push_back(std::move(e));
        // Trimmed from the FRONT, so what is kept is always the most recent
        // window.
        while (g_entries.size() > CAPACITY) g_entries.pop_front();
    }

    // OUTSIDE the lock. PostMessage does not block, but holding a mutex across
    // any call into USER32 from a socket thread is how a deadlock gets written.
    NotifyChanged();
}

void NotifyChanged() {
    // Nothing to do when no panel is open, which is the usual case even with
    // recording on.
    HWND hwnd = g_notifyWnd.load(std::memory_order_acquire);
    if (!hwnd) return;

    // The coalescing gate: exchange returns the OLD value, so exactly one
    // producer in a burst gets false and posts.
    if (g_notifyPending.exchange(true, std::memory_order_acq_rel)) return;

    if (!PostMessageW(hwnd, Constants::WM_QIV_REMOTE_LOG_ADDED, 0, 0)) {
        // The window went away between the load and the post. Release the gate
        // or every later entry would see a pending notification that will never
        // arrive, and the panel would stop updating for the rest of the session.
        g_notifyPending.store(false, std::memory_order_release);
    }
}

std::vector<Entry> Snapshot() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return std::vector<Entry>(g_entries.begin(), g_entries.end());
}

size_t Count() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_entries.size();
}

void SetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool IsEnabled()         { return g_enabled.load(std::memory_order_relaxed); }

void SetNotifyWindow(HWND hwnd) {
    g_notifyWnd.store(hwnd, std::memory_order_release);
    // Unregistering leaves the gate closed otherwise, and the next panel to
    // open would never be told about anything.
    if (!hwnd) g_notifyPending.store(false, std::memory_order_release);
}

void ClearNotifyPending() {
    g_notifyPending.store(false, std::memory_order_release);
}

void SetSelfName(const std::wstring &name) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_selfName = name;
}

std::wstring SelfName() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_selfName.empty() ? std::wstring(L"(this)") : g_selfName;
}

void Clear() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_entries.clear();
}

// =============================================================================
// Formatting
// =============================================================================
std::wstring FormatTime(long long whenFt) {
    if (whenFt <= 0) return L"—";

    FILETIME utc;
    utc.dwLowDateTime  = static_cast<DWORD>(whenFt & 0xFFFFFFFF);
    utc.dwHighDateTime = static_cast<DWORD>((whenFt >> 32) & 0xFFFFFFFF);

    FILETIME local{};
    if (!FileTimeToLocalFileTime(&utc, &local)) return L"—";

    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&local, &st)) return L"—";

    wchar_t b[32];
    swprintf_s(b, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

std::wstring FormatDelta(long long us) {
    if (us < 0) return L"—";
    wchar_t b[32];
    // Three bands, because the interesting range spans five orders of
    // magnitude: loopback is sub-millisecond, a LAN screen is single-digit
    // milliseconds, and a target that is swapping is seconds.
    if (us < 1000)         swprintf_s(b, L"%lld µs", us);
    else if (us < 1000000) swprintf_s(b, L"%.1f ms", static_cast<double>(us) / 1000.0);
    else                   swprintf_s(b, L"%.2f s",  static_cast<double>(us) / 1000000.0);
    return b;
}

// =============================================================================
// Disk
// =============================================================================
bool SaveTo(const std::wstring &path, std::wstring &errorOut) {
    const std::vector<Entry> rows = Snapshot();

    // Built whole, then written in one go: a partially written log that was
    // interrupted mid-flush is a file that loads as garbage, and the whole
    // buffer is a few megabytes at CAPACITY.
    std::wstring text = FILE_HEADER;
    text += L"\r\n";
    for (const Entry &e : rows) {
        text += std::to_wstring(e.seq);      text += L'\t';
        text += std::to_wstring(e.whenFt);   text += L'\t';
        text += std::to_wstring(e.deltaUs);  text += L'\t';
        text += (e.dir == Direction::Out) ? L"OUT" : L"IN";
        text += L'\t';
        text += Flatten(e.sender);           text += L'\t';
        text += Flatten(e.command);          text += L'\t';
        text += Flatten(e.receiver);         text += L'\t';
        text += Flatten(e.response);
        text += L"\r\n";
    }

    const std::string utf8 = ToUtf8(text);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        errorOut = LastErrorSentence(L"Could not create the log file", path);
        return false;
    }

    // BOM, so Notepad and Excel both read it as UTF-8 rather than guessing at
    // the code page — a log full of machine names is exactly where that bites.
    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    bool ok = WriteFile(h, bom, 3, &written, nullptr) != 0;
    if (ok && !utf8.empty())
        ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) != 0 &&
             written == static_cast<DWORD>(utf8.size());

    if (!ok) errorOut = LastErrorSentence(L"Could not write the log file", path);
    CloseHandle(h);
    return ok;
}

bool LoadFrom(const std::wstring &path, std::wstring &errorOut) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        errorOut = LastErrorSentence(L"Could not open that file", path);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(h);
        errorOut = L"That file is empty, or too large to be a qIV remote log "
                   L"(the limit is 64 MB).";
        return false;
    }

    std::string raw(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr) != 0;
    CloseHandle(h);
    if (!ok) {
        errorOut = LastErrorSentence(L"Could not read that file", path);
        return false;
    }
    raw.resize(read);

    size_t off = 0;
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF)
        off = 3;

    const std::wstring text = FromUtf8(raw.data() + off, raw.size() - off);

    std::vector<Entry> loaded;
    bool headerSeen = false;

    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) { if (start > text.size()) break; continue; }

        if (!headerSeen) {
            if (line != FILE_HEADER) {
                errorOut = L"That is not a qIV remote log, or it was written by a "
                           L"different version.\r\n\r\nThe first line must be the "
                           L"header this build writes.";
                return false;
            }
            headerSeen = true;
            continue;
        }

        const std::vector<std::wstring> f = SplitTabs(line);
        if (f.size() < 8) continue; // a truncated tail line, not a reason to fail

        Entry e;
        e.seq      = ToLL(f[0]);
        e.whenFt   = ToLL(f[1]);
        e.deltaUs  = ToLL(f[2]);
        e.dir      = (f[3] == L"IN") ? Direction::In : Direction::Out;
        e.sender   = f[4];
        e.command  = f[5];
        e.receiver = f[6];
        e.response = f[7];
        loaded.push_back(std::move(e));
    }

    if (!headerSeen) {
        errorOut = L"That file has no qIV remote-log header.";
        return false;
    }

    // REPLACE, not merge — see the header. The sequence counter is pushed past
    // whatever was loaded, so a live exchange arriving afterwards cannot be
    // given a number the file already used.
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_entries.assign(loaded.begin(), loaded.end());
        while (g_entries.size() > CAPACITY) g_entries.pop_front();
        for (const Entry &e : g_entries)
            if (e.seq >= g_nextSeq) g_nextSeq = e.seq + 1;
    }
    return true;
}

} // namespace Remote::Log
