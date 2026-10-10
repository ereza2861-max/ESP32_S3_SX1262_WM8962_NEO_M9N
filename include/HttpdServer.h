#pragma once
#include <Arduino.h>
#include <functional>
#include <esp_http_server.h>
#include <esp_https_server.h>

class HttpdRequest;
class HttpdResponse;

// Native ESP-IDF esp_https_server wrapper for FieldRadio WebUI.
//
// Design constraints:
//  * esp_https_server has no equivalent of Arduino WebServer::on(uri, method, fn).
//    Every endpoint must be registered as a separate httpd_uri_t entry with a
//    C-compatible handler. We wrap the C handler in a static trampoline that
//    recovers the std::function from httpd_uri_t.user_ctx.
//  * httpd_uri_t.uri is a const char* that must stay alive for the lifetime of
//    the server. We strdup() each URI and free it in the destructor.
//  * httpd_uri_t.user_ctx is void*, so we heap-allocate one Handler per
//    endpoint and free it in the destructor.
//  * httpd_ssl_config_t.httpd.max_uri_handlers defaults to 8. The FieldRadio
//    WebUI registers 121 endpoints, so we must raise this to a safe value or
//    every endpoint past the 8th will silently fail to register.
class HttpdServer {
public:
  using Handler = std::function<void(HttpdRequest&, HttpdResponse&)>;

  // Upper bound chosen to comfortably exceed the WebUI endpoint count with a
  // little headroom. Exceeding this number is a hard registration failure and
  // is reported through registerFailures_.
  static constexpr size_t MAX_HANDLERS = 160;

  HttpdServer() = default;
  ~HttpdServer();
  HttpdServer(const HttpdServer&) = delete;
  HttpdServer& operator=(const HttpdServer&) = delete;

  // Register one endpoint. Returns false if the internal table is full or
  // strdup/allocation fails. The handler is moved into internal storage.
  bool on(const char* uri, httpd_method_t method, Handler handler);

  // Start the TLS server with DER-encoded certificate and key material.
  // Returns false on any failure (already started, config error, httpd_ssl_start
  // failure, or any endpoint registration failure).
  bool begin(const uint8_t* certDer, size_t certLen,
             const uint8_t* keyDer, size_t keyLen, uint16_t port);

  // Stop the server and release the handler table. Safe to call multiple times.
  void stop();

  bool started() const { return handle_ != nullptr; }
  size_t registeredCount() const { return entryCount_; }
  size_t registerFailures() const { return registerFailures_; }

private:
  struct Entry {
    httpd_uri_t uri{};
    Handler* handler = nullptr;
    char* uriCopy = nullptr;
  };

  static esp_err_t trampoline(httpd_req_t* req);

  httpd_handle_t handle_ = nullptr;
  Entry entries_[MAX_HANDLERS]{};
  size_t entryCount_ = 0;
  size_t registerFailures_ = 0;
};
