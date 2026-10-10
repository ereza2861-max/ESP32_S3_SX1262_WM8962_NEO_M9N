#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <esp_http_server.h>
#include <map>
#include <memory>

// Thin C++ view over httpd_req_t.
//
// Behaviour mirrors the old ESPWebServer compat layer as closely as possible
// so that WebUi handlers do not need semantic changes:
//   * arg(name)  -> query string first, then body urlencoded (parsed once)
//   * header(n)  -> HTTP header value, case-insensitive
//   * contentLength() -> raw Content-Length field
//   * bodyText()  -> raw body (rejects > maxBytes)
//
// One HttpdRequest instance exists per active request and is destroyed when
// the trampoline returns. No state is shared between requests.
class HttpdRequest {
public:
  explicit HttpdRequest(httpd_req_t* req) : req_(req) {}

  const char* uri() const { return req_ ? req_->uri : ""; }
  httpd_method_t method() const {
    return req_ ? static_cast<httpd_method_t>(req_->method) : HTTP_GET;
  }
  size_t contentLength() const { return req_ ? req_->content_len : 0; }
  httpd_req_t* raw() const { return req_; }

  // Header lookup by case-insensitive name. Returns empty String if missing.
  String header(const char* name) const;

  // Unified argument lookup. Checks query string first, then POST body
  // (application/x-www-form-urlencoded). Body is parsed exactly once on
  // first call and cached for subsequent calls.
  String arg(const char* name) const;
  bool hasArg(const char* name) const;

  // Query-string-only lookup, used when a handler must not see body fields.
  String query(const char* name) const;
  bool hasQuery(const char* name) const;

  // Raw body. Rejects bodies larger than maxBytes with return false.
  // Used by /api/message, /api/messages/reply, /api/config/restore, etc.
  bool bodyText(String& out, size_t maxBytes) const;

  // Remote peer address. Returns 0.0.0.0 on failure.
  IPAddress remoteIp() const;

private:
  void ensureBodyParsed() const;

  httpd_req_t* req_;
  mutable bool bodyParsed_ = false;
  mutable bool bodyOverflow_ = false;
  mutable std::map<String, String> bodyFields_;
};
