#include "HttpdMultipart.h"
#include "HttpdRequest.h"

#include <cstring>
#include <memory>

namespace {

// Read up to `want` bytes from the request body into `out`.
// Returns number of bytes read (>=0) or -1 on error.
// A return value of 0 with `remaining == 0` means clean end of body.
int recvChunk(HttpdRequest& req, uint8_t* out, size_t want, size_t& remaining) {
  if (remaining == 0) return 0;
  if (want > remaining) want = remaining;
  if (want == 0) return 0;
  int got = httpd_req_recv(req.raw(), reinterpret_cast<char*>(out), want);
  if (got <= 0) {
    // ESP_ERR_HTTPD_RECV_TIMEOUT is not fatal by itself, but in this parser
    // we treat any short read as end-of-body failure because the boundary
    // framing has already been lost.
    return -1;
  }
  remaining -= static_cast<size_t>(got);
  return got;
}

// Locate the first occurrence of `needle` in `hay` of length `hayLen`.
// Returns index or -1.
int findBytes(const uint8_t* hay, size_t hayLen, const uint8_t* needle, size_t needleLen) {
  if (needleLen == 0 || hayLen < needleLen) return -1;
  for (size_t i = 0; i + needleLen <= hayLen; ++i) {
    if (memcmp(hay + i, needle, needleLen) == 0) return static_cast<int>(i);
  }
  return -1;
}

String trimQuotes(const String& s) {
  String r = s;
  r.trim();
  if (r.startsWith("\"") && r.endsWith("\"") && r.length() >= 2) {
    r = r.substring(1, r.length() - 1);
  }
  return r;
}

}  // namespace

bool HttpdMultipart::parse(HttpdRequest& req, size_t maxTotalBytes,
                           FieldCallback fieldCb, FileCallback fileCb) {
  if (req.raw() == nullptr) return false;

  // ---- 1. Extract boundary from Content-Type ----
  String contentType = req.header("Content-Type");
  if (contentType.isEmpty()) return false;
  int bpos = contentType.indexOf("boundary=");
  if (bpos < 0) return false;
  String boundary = contentType.substring(bpos + 9);
  int semi = boundary.indexOf(';');
  if (semi >= 0) boundary = boundary.substring(0, semi);
  boundary = trimQuotes(boundary);
  if (boundary.isEmpty() || boundary.length() > HTTPD_MULTIPART_MAX_BOUNDARY_BYTES) {
    return false;
  }

  // Full delimiter used between parts: "--" + boundary
  String delim = "--" + boundary;
  // Closing delimiter: "--" + boundary + "--"
  String closing = delim + "--";

  // Body size guard.
  if (req.contentLength() == 0 || req.contentLength() > maxTotalBytes) {
    return false;
  }

  // ---- 2. State machine ----
  size_t remaining = req.contentLength();

  // Rolling buffer large enough to hold the boundary plus a small window.
  // We grow it only as needed.
  constexpr size_t WINDOW = 1024;
  uint8_t buf[WINDOW];
  size_t bufLen = 0;

  auto fill = [&]() -> bool {
    if (bufLen >= WINDOW) return false;  // should never happen
    int got = recvChunk(req, buf + bufLen, WINDOW - bufLen, remaining);
    if (got < 0) return false;
    bufLen += static_cast<size_t>(got);
    return true;
  };

  // Prime the buffer: must start with delim + CRLF.
  if (!fill()) return false;
  String expectedStart = delim + "\r\n";
  if (bufLen < expectedStart.length() ||
      memcmp(buf, expectedStart.c_str(), expectedStart.length()) != 0) {
    // Some clients omit the leading CRLF; tolerate it.
    if (bufLen < delim.length() ||
        memcmp(buf, delim.c_str(), delim.length()) != 0) {
      return false;
    }
    // Advance past delim.
    memmove(buf, buf + delim.length(), bufLen - delim.length());
    bufLen -= delim.length();
    if (bufLen < 2 || buf[0] != '\r' || buf[1] != '\n') return false;
    memmove(buf, buf + 2, bufLen - 2);
    bufLen -= 2;
  } else {
    memmove(buf, buf + expectedStart.length(), bufLen - expectedStart.length());
    bufLen -= expectedStart.length();
  }

  // ---- 3. Parse each part ----
  for (;;) {
    // --- 3a. Read headers up to CRLFCRLF ---
    String headerBlock;
    headerBlock.reserve(256);
    bool headersDone = false;
    while (!headersDone) {
      int crlfcrlf = -1;
      for (size_t i = 0; i + 4 <= bufLen; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
          crlfcrlf = static_cast<int>(i);
          break;
        }
      }
      if (crlfcrlf >= 0) {
        if (headerBlock.length() + static_cast<size_t>(crlfcrlf) >
            HTTPD_MULTIPART_MAX_HEADER_BYTES) return false;
        headerBlock += String(reinterpret_cast<char*>(buf), crlfcrlf);

        memmove(buf, buf + crlfcrlf + 4, bufLen - crlfcrlf - 4);
        bufLen -= crlfcrlf + 4;
        headersDone = true;
      } else {
        // Consume current buffer into headerBlock and refill.
        if (headerBlock.length() + bufLen > HTTPD_MULTIPART_MAX_HEADER_BYTES)
          return false;
        headerBlock += String(reinterpret_cast<char*>(buf), bufLen);
        bufLen = 0;
        if (remaining == 0) return false;
        if (!fill()) return false;
      }
    }

    // --- 3b. Parse Content-Disposition / Content-Type ---
    String fieldName;
    String fieldFilename;
    String fieldMime;
    {
      int lineStart = 0;
      while (lineStart < static_cast<int>(headerBlock.length())) {
        int lineEnd = headerBlock.indexOf("\r\n", lineStart);
        if (lineEnd < 0) lineEnd = headerBlock.length();
        String line = headerBlock.substring(lineStart, lineEnd);
        lineStart = lineEnd + 2;
        int colon = line.indexOf(':');
        if (colon < 0) continue;
        String key = line.substring(0, colon);
        String val = line.substring(colon + 1);
        key.trim();
        val.trim();
        if (key.equalsIgnoreCase("Content-Disposition")) {
          int npos = val.indexOf("name=");
          if (npos >= 0) {
            String rest = val.substring(npos + 5);
            int end = rest.indexOf(';');
            fieldName = trimQuotes(end < 0 ? rest : rest.substring(0, end));
          }
          int fpos = val.indexOf("filename=");
          if (fpos >= 0) {
            String rest = val.substring(fpos + 9);
            int end = rest.indexOf(';');
            fieldFilename = trimQuotes(end < 0 ? rest : rest.substring(0, end));
          }
        } else if (key.equalsIgnoreCase("Content-Type")) {
          fieldMime = val;
        }
      }
    }

    const bool isFile = !fieldFilename.isEmpty();

    // --- 3c. Stream field data until the next boundary ---
    // The terminator can be either "\r\n" + delim (another part) or
    // "\r\n" + closing (end of body). We scan for "\r\n--boundary    // then decide based on the two bytes that follow.
    String scan = "\r\n" + delim;
    const uint8_t* scanPtr = reinterpret_cast<const uint8_t*>(scan.c_str());
    const size_t scanLen = scan.length();

    bool firstChunk = true;
    bool aborted = false;

    for (;;) {
      // Ensure we have at least scanLen bytes buffered, or we are at end.
      while (bufLen < scanLen && remaining > 0) {
        if (!fill()) { aborted = true; break; }
      }
      if (aborted) break;

      int hit = findBytes(buf, bufLen, scanPtr, scanLen);
      if (hit < 0) {
        // No boundary in buffer. Emit everything except the last (scanLen-1)
        // bytes (which could be the start of a boundary spanning the next
        // refill).
        size_t safe = (bufLen > scanLen) ? bufLen - (scanLen - 1) : 0;
        if (safe > 0) {
          if (isFile) {
            if (!fileCb(fieldName.c_str(), fieldFilename.c_str(),
                        fieldMime.c_str(), buf, safe,
                        firstChunk, false)) { aborted = true; break; }
          } else {
            if (!fieldCb(fieldName.c_str(), fieldMime.c_str(),
                         buf, safe)) { aborted = true; break; }
          }
          firstChunk = false;
          memmove(buf, buf + safe, bufLen - safe);
          bufLen -= safe;
        }
        if (remaining == 0) {
          // Truncated body: no closing boundary seen.
          aborted = true;
          break;
        }
        if (!fill()) { aborted = true; break; }
        continue;
      }

      // Boundary found at hit.
      // Data before boundary is field payload.
      if (hit > 0) {
        if (isFile) {
          if (!fileCb(fieldName.c_str(), fieldFilename.c_str(),
                      fieldMime.c_str(), buf, static_cast<size_t>(hit),
                      firstChunk, false)) { aborted = true; break; }
        } else {
          if (!fieldCb(fieldName.c_str(), fieldMime.c_str(),
                       buf, static_cast<size_t>(hit))) { aborted = true; break; }
        }
        firstChunk = false;
      }

      // Move buffer past "\r\n" + delim.
      const size_t afterBoundary = static_cast<size_t>(hit) + scanLen;
      memmove(buf, buf + afterBoundary, bufLen - afterBoundary);
      bufLen -= afterBoundary;

      // Ensure we have two bytes after the boundary.
      while (bufLen < 2 && remaining > 0) {
        if (!fill()) { aborted = true; break; }
      }
      if (aborted) break;
      if (bufLen < 2) { aborted = true; break; }

      const bool isClosing = (buf[0] == '-' && buf[1] == '-');
      // Consume the two bytes after boundary ("--" or CRLF).
      memmove(buf, buf + 2, bufLen - 2);
      bufLen -= 2;

      if (isClosing) {
        // Final chunk for this field.
        if (isFile) {
          if (!fileCb(fieldName.c_str(), fieldFilename.c_str(),
                      fieldMime.c_str(), nullptr, 0,
                      firstChunk, true)) { aborted = true; break; }
        }
        // Drain trailing CRLF if present.
        while (bufLen > 0 && (buf[0] == '\r' || buf[0] == '\n')) {
          memmove(buf, buf + 1, bufLen - 1);
          bufLen -= 1;
        }
        // Done.
        return true;
      }

      // Not closing: expect CRLF after the boundary. Consume it.
      if (bufLen < 2) {
        // Need more data for CRLF.
        while (bufLen < 2 && remaining > 0) {
          if (!fill()) { aborted = true; break; }
        }
        if (aborted || bufLen < 2) { aborted = true; break; }
      }
      if (buf[0] != '\r' || buf[1] != '\n') { aborted = true; break; }
      memmove(buf, buf + 2, bufLen - 2);
      bufLen -= 2;

      // Notify file callback about last chunk *before* starting the next part.
      if (isFile) {
        if (!fileCb(fieldName.c_str(), fieldFilename.c_str(),
                    fieldMime.c_str(), nullptr, 0,
                    firstChunk, true)) { aborted = true; break; }
      }
      // Break inner loop; continue outer for the next part.
      break;
    }

    if (aborted) return false;
  }
}
