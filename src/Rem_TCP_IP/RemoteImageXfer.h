#pragma once
#include <string>
#include <vector>

// =============================================================================
// RemoteImageXfer — moving one PICTURE over the wire, in either direction.
//
// WHY BYTES AND NOT A PATH. `ShowImageOnce <path>` costs nothing and works
// perfectly — on one machine. Two machines do not share a filesystem, and the
// whole point of Alt+Enter (stream out) and Ctrl+Alt+Enter (stream in) is that
// the screen may be in another room. So the file's OWN BYTES travel and the far
// end decodes them with its own WIC, exactly as if it had opened the file.
//
// The FILE'S bytes, not its pixels. A decoded frame is ten to fifty times larger
// than the JPEG it came from, and the receiving end already owns a decoder for
// every format this program can open. The file NAME travels with them for one
// reason only: its extension is what tells that decoder which format it is
// looking at. It is not a path and is never treated as one.
//
// FRAMING. The protocol is line-based with a bounded line length, so:
//
//   OUT  a request cannot be multi-line, so the transfer is a SEQUENCE of
//        ordinary commands — StreamImageBegin, StreamImageChunk × n,
//        StreamImageShow — each with its own reply. Driven from a mirror sender
//        thread, which is the only thread allowed to wait for one.
//
// WRITTEN TO BE IMPLEMENTED BY OTHER CLIENTS, not just by this program driving
// itself: a phone app that sends a picture to a screen, or asks a screen what it
// is showing, is a first-class caller here. Hence plain base64 over the existing
// text protocol, a declared byte COUNT in StreamImageBegin so the receiver can
// prove the transfer arrived whole rather than assume it, a size and name on the
// reply of the inbound form, and every row carrying its own description in the
// `help` listing. Nothing about the sequence requires the caller to be qIV.
//
//   IN   a REPLY may be several lines (the `help` verb already is), so
//        `SendDisplayedImage` answers with its body lines and then its OK, and
//        needs no framing commands at all.
//
// Both directions end in the same place: bytes written to a temp file, which is
// then displayed through AppState::Interjection. Going via a file rather than a
// memory decode means the whole existing decode/cache/display path is reused
// unchanged — and a one-shot picture is worth exactly one temp file.
// =============================================================================
namespace Remote::Xfer {

    // --- Sending side --------------------------------------------------------

    // Reads a whole image file. Refuses anything over STREAM_MAX_BYTES, and says
    // which of "cannot read" and "too large" happened, because the remedies differ.
    bool ReadImageFile(const std::wstring &path, std::vector<unsigned char> &out,
                       std::wstring &errOut);

    // The command sequence that carries `bytes` to a peer:
    //
    //   StreamImageBegin <totalBytes> <fileName>
    //   StreamImageChunk <base64>            × ceil(total / STREAM_CHUNK_BYTES)
    //   StreamImageShow
    //
    // Built as whole lines so the caller only has to send them in order. The count
    // comes first because the receiver acts on it before any bytes arrive: refuse
    // an oversize transfer up front, reserve once, and prove at the end that what
    // arrived is what was promised.
    std::vector<std::wstring> BuildStreamCommands(const std::wstring &fileName,
                                                  const std::vector<unsigned char> &bytes);

    // Decodes `path`, scales it to fit `maxDim` on its longest edge, and encodes
    // the result as JPEG. False with a reason on any failure.
    //
    // NEVER UPSCALES: a picture already smaller than maxDim is re-encoded at its
    // own size. Enlarging would send more bytes than the original to show the
    // same detail, which is the opposite of the point.
    //
    // THREAD-SAFE, and it has to be — this runs on a socket thread, not the UI
    // thread. It creates its OWN WIC factory rather than touching app.wicFactory,
    // for the same reason the decoder workers do: a COM object created on one
    // apartment is not free to use from another. Requires CoInitializeEx on the
    // calling thread.
    bool BuildPreviewJpeg(const std::wstring &path, int maxDim, int quality,
                          std::vector<unsigned char> &out, std::wstring &errOut);

    // The REPLY form, for answering `SendDisplayedImage`: base64 body lines each
    // prefixed with the data marker, followed by the caller's own OK line. One
    // string with embedded CRLFs — the transport sends it as written and the
    // receiving Client accumulates lines until the OK.
    std::wstring BuildDataReplyBody(const std::vector<unsigned char> &bytes);

    // --- Receiving side ------------------------------------------------------

    // Pulls the base64 out of an accumulated `SendDisplayedImage` reply (the body
    // lines) and decodes it. False when the reply carried no body or the base64
    // was truncated in transit.
    bool ParseDataReply(const std::wstring &reply, std::vector<unsigned char> &out);

    // The file name a `SendDisplayedImage` reply reported, from its OK line.
    // Empty when it did not name one — the caller then invents an extension,
    // because without one no decoder can be chosen.
    std::wstring FileNameFromDataReply(const std::wstring &reply);

    // Writes `bytes` to a uniquely-named file in the user's temp folder, keeping
    // `fileName`'s extension so the decoder can be selected from it. The caller
    // owns the result and must delete it — ClearInterjection does exactly that.
    bool WriteTempImage(const std::wstring &fileName,
                        const std::vector<unsigned char> &bytes,
                        std::wstring &pathOut);

    // The marker that opens a base64 body line, and the length of it. Exposed so
    // both the builder and the parser use one spelling.
    extern const wchar_t *DATA_PREFIX;

} // namespace Remote::Xfer
