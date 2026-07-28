#pragma once
#include <windows.h>
#include <string>
#include <vector>

// =============================================================================
// RemoteClient — the "connect to another instance" half.
//
// One qIV driving another. The panel fills in a target address, port and
// password, and this opens the conversation the server side already speaks.
//
// -----------------------------------------------------------------------------
// BLOCKING, BUT ALWAYS BOUNDED — and never on the UI thread.
//
// Every call here can block for up to its timeout: connecting to a host that is
// switched off takes as long as the TCP stack wants unless something stops it.
// So every wait is explicitly capped:
//
//   • connect() is issued on a NON-BLOCKING socket and waited on with select(),
//     so an unreachable host fails in CONNECT_TIMEOUT_MS instead of ~20 seconds
//   • SO_RCVTIMEO/SO_SNDTIMEO bound every later read and write
//
// Even so, the caller must not be the UI thread — a bounded stall is still a
// stall, and freezing the viewer for several seconds because a wall screen is
// unplugged would be unacceptable. Slice 6's panel runs these on a short-lived
// worker and posts the result back.
// -----------------------------------------------------------------------------
//
// Full design record: docs/REMOTE_TCP_IP_SPEC.md
// =============================================================================

namespace Remote {

    class Client {
    public:
        Client() = default;
        ~Client();
        Client(const Client &)            = delete;
        Client &operator=(const Client &) = delete;

        // Opens the connection, reads the server banner and completes the
        // challenge-response if the peer asks for one. Returns false with a
        // human-readable reason in `errorOut`.
        //
        // `password` is the PLAINTEXT the user typed. It is used to answer the
        // challenge and is not retained after Connect returns.
        bool Connect(const std::wstring &host, int port,
                     const std::wstring &password, std::wstring &errorOut);

        // Sends one command line and returns the peer's reply verbatim
        // (including its "OK"/"ERR" prefix, which the caller may want to show).
        //
        // EVENT lines arriving mid-exchange are NOT part of the reply. An
        // observed peer pushes them whenever it feels like it, including between
        // our request and its answer, so they are pulled out here and appended
        // to `eventsOut` instead of being folded into `replyOut` — which is what
        // the multi-line `help` accumulation would otherwise do to them.
        bool Send(const std::wstring &commandLine,
                  std::wstring &replyOut, std::wstring &errorOut,
                  std::vector<std::wstring> *eventsOut = nullptr);

        // Bounded read for an unsolicited line while no request is outstanding.
        // Returns true when one arrived, false on timeout (not an error) or on
        // a dead connection — check IsConnected() to tell those apart.
        //
        // Needed because an observed peer talks when IT acts, not when we ask.
        // Without this the socket would only be read during a Send, and a slave
        // whose slideshow advanced while we were idle would go unheard until we
        // happened to send something.
        bool PollLine(std::wstring &lineOut, int timeoutMs);

        void Disconnect();
        bool IsConnected() const;

        // The banner the server sent on connect — app version, protocol version
        // and the instance Name when it has one.
        const std::wstring &Banner() const { return m_banner; }

    private:
        // UINT_PTR rather than SOCKET so this header does not have to drag
        // winsock2.h into everything that includes it — and winsock2.h must
        // precede windows.h, which would put an ordering trap in every consumer.
        UINT_PTR     m_sock = static_cast<UINT_PTR>(~0ull); // INVALID_SOCKET
        std::string  m_accum;                                // partial line buffer
        std::wstring m_banner;
        bool         m_connected = false;
    };

    // One-shot reachability probe for the panel's "Check Connection" button:
    // connect, authenticate, `ping`, disconnect. Leaves nothing open.
    // On success `infoOut` carries the peer's banner.
    bool Probe(const std::wstring &host, int port, const std::wstring &password,
               std::wstring &infoOut, std::wstring &errorOut);

} // namespace Remote
