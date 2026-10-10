#pragma once
#include <Arduino.h>
#include <functional>
#include <esp_err.h>

class HttpdRequest;

// Streaming parser for multipart/form-data bodies.
//
// This parser is intentionally minimal: it handles the exact shape emitted by
// the FieldRadio WebUI (/api/upload with a single WAV file field, plus any
// non-file fields the browser includes). It does not support quoted-printable
// or base64 transfer encodings, nested multipart, or fields larger than
// maxTotalBytes.
//
// The parser consumes the entire request body exactly once. After parse()
// returns, HttpdRequest::bodyText() and HttpdRequest::arg() must not be used
// on the same request.
class HttpdMultipart {
public:
  // Non-file field chunk. name is the form field name, mime is the field
  // Content-Type if present (empty otherwise). data/len point to a single
  // chunk (may be called multiple times for large fields). Returning false
  // aborts the whole parse.
  using FieldCallback = std::function<bool(const char* name,
                                           const char* mime,
                                           const uint8_t* data,
                                           size_t len)>;

  // File field callback. firstChunk is true on the very first call for a
  // given field, lastChunk is true on the very last call. data/len point to
  // a single chunk. Returning false aborts the parse and the caller is
  // responsible for cleaning up any partial file.
  using FileCallback = std::function<bool(const char* name,
                                          const char* filename,
                                          const char* mime,
                                          const uint8_t* data,
                                          size_t len,
                                          bool firstChunk,
                                          bool lastChunk)>;

  // Parse the body of req. Returns true if the whole body was consumed and
  // every callback returned true. Returns false on malformed input, missing
  // boundary, body exceeding maxTotalBytes, or callback abort.
  bool parse(HttpdRequest& req, size_t maxTotalBytes,
             FieldCallback fieldCb, FileCallback fileCb);

private:
  // Internal state machine helper: read up to n bytes into buf from the
  // underlying request. Returns -1 on error, 0 on clean end of body.
  struct Reader {
    HttpdRequest* req = nullptr;
    size_t remaining = 0;
  };
};

// Maximum size of a single field header block (Content-Disposition plus
// Content-Type plus a few extra headers). Larger blocks are rejected.
constexpr size_t HTTPD_MULTIPART_MAX_HEADER_BYTES = 1024;

// Maximum size of the boundary string. RFC 2046 limits to 70 chars.
constexpr size_t HTTPD_MULTIPART_MAX_BOUNDARY_BYTES = 72;
