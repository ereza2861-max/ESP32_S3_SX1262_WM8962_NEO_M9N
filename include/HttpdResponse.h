#pragma once
#include <Arduino.h>
#include <FS.h>
#include <esp_http_server.h>

// Thin C++ view over the httpd response side.
//
// send*() must be called at most once per request. A second call is ignored
// (with a warning) because esp_http_server returns ESP_ERR_HTTPD_RESP_SEND
// after the first header block and may leave the connection half-closed.
class HttpdResponse {
public:
  explicit HttpdResponse(httpd_req_t* req) : req_(req) {}

  // Header helpers. setHeader() must be called before any send*().
  void setHeader(const char* name, const char* value);
  void setHeader(const char* name, const String& value);
  void setType(const char* mime);

  // Body senders.
  void send(int code, const char* mime, const String& body);
  void send(int code, const char* mime, const char* body);
  void sendEmpty(int code);
  void sendJson(int code, const String& body);
  void sendHtml(int code, const String& body);
  void sendText(int code, const String& body);
  void sendCsv(int code, const String& body);

  // Convenience for common auth flows.
  void send401Basic(const char* realm, const char* message);
  void send403(const char* message);
  void send404(const char* message);
  void send413(const char* message);
  void send429(const char* message);
  void send500(const char* message);
  void send503(const char* message);

  // Stream a file in bounded chunks using HTTP chunked transfer encoding.
  bool streamFile(File& file, const char* mime);

  bool responded() const { return responded_; }
  httpd_req_t* raw() const { return req_; }

private:
  httpd_req_t* req_;
  bool responded_ = false;
  bool warnedDouble_ = false;
};
