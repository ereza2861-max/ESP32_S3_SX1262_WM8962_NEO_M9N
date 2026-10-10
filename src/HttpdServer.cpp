#include "HttpdServer.h"
#include "HttpdRequest.h"
#include "HttpdResponse.h"

#include <cstring>
#include <cstdlib>
#include <new>
#include <utility>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

HttpdServer::~HttpdServer() {
  stop();
  for (size_t i = 0; i < entryCount_; ++i) {
    delete entries_[i].handler;
    entries_[i].handler = nullptr;
    free(entries_[i].uriCopy);
    entries_[i].uriCopy = nullptr;
  }
  entryCount_ = 0;
}

bool HttpdServer::on(const char* uri, httpd_method_t method, Handler handler) {
  if (uri == nullptr || handler == nullptr) return false;
  if (entryCount_ >= MAX_HANDLERS) {
    ++registerFailures_;
    Serial.printf("HttpdServer: handler table full at %s\n", uri);
    return false;
  }
  char* copy = strdup(uri);
  if (copy == nullptr) {
    ++registerFailures_;
    Serial.printf("HttpdServer: strdup failed for %s\n", uri);
    return false;
  }
  Handler* boxed = new (std::nothrow) Handler(std::move(handler));
  if (boxed == nullptr) {
    free(copy);
    ++registerFailures_;
    Serial.printf("HttpdServer: handler alloc failed for %s\n", uri);
    return false;
  }

  Entry& e = entries_[entryCount_];
  e.uriCopy = copy;
  e.handler = boxed;
  e.uri.uri = copy;
  e.uri.method = method;
  e.uri.handler = &HttpdServer::trampoline;
  e.uri.user_ctx = boxed;
  // is_websocket and handle_ws are zero-initialised by Entry{}.
  ++entryCount_;
  return true;
}

esp_err_t HttpdServer::trampoline(httpd_req_t* req) {
  if (req == nullptr) return ESP_FAIL;
  if (req->user_ctx == nullptr) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "handler context missing");
  }
  auto* handler = static_cast<Handler*>(req->user_ctx);
  HttpdRequest request(req);
  HttpdResponse response(req);
  (*handler)(request, response);
  return ESP_OK;
}

bool HttpdServer::begin(const uint8_t* certDer, size_t certLen,
                        const uint8_t* keyDer, size_t keyLen, uint16_t port) {
  if (handle_ != nullptr) {
    Serial.println("HttpdServer: already started");
    return false;
  }
  if (certDer == nullptr || certLen == 0 || keyDer == nullptr || keyLen == 0) {
    Serial.println("HttpdServer: missing TLS material");
    return false;
  }
  if (entryCount_ == 0) {
    Serial.println("HttpdServer: no handlers registered before begin()");
    return false;
  }

  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  // Raise from the ESP-IDF default of 8; WebUI needs 121.
  config.httpd.max_uri_handlers = static_cast<uint16_t>(MAX_HANDLERS);
  // Each TLS socket costs ~40 KB of internal RAM. Keep this small: the WebUI
  // is single-operator and does not need concurrent sessions.
  config.httpd.max_open_sockets = 4;
  // WebUI handlers do SD I/O and build JSON responses; raise the task stack
  // above the 10 KB default to avoid stack overflow under handler load.
  config.httpd.stack_size = 12288;
  config.httpd.task_priority = tskIDLE_PRIORITY + 5;
  config.httpd.lru_purge_enable = true;
  config.httpd.recv_wait_timeout = 5;
  config.httpd.send_wait_timeout = 5;

  config.servercert = certDer;
  config.servercert_len = certLen;
  config.prvtkey_pem = keyDer;
  config.prvtkey_len = keyLen;
  config.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
  config.port_secure = port;
  // session_tickets and use_secure_element stay at their defaults (false).

  esp_err_t err = httpd_ssl_start(&handle_, &config);
  if (err != ESP_OK) {
    Serial.printf("HttpdServer: httpd_ssl_start failed: %s\n", esp_err_to_name(err));
    handle_ = nullptr;
    return false;
  }

  // Register endpoints after the server is listening. Registration itself is
  // synchronous and safe to perform from the caller task.
  size_t failedOnStart = 0;
  for (size_t i = 0; i < entryCount_; ++i) {
    Entry& e = entries_[i];
    esp_err_t reg = httpd_register_uri_handler(handle_, &e.uri);
    if (reg != ESP_OK) {
      Serial.printf("HttpdServer: register %s failed: %s\\n",
                    e.uriCopy, esp_err_to_name(reg));
      ++registerFailures_;
      ++failedOnStart;
    }
  }

  const size_t registered = entryCount_ - failedOnStart;
  Serial.printf("HttpdServer: started on port %u with %u/%u handlers (%u failures)\\n",
                static_cast<unsigned>(port),
                static_cast<unsigned>(registered),
                static_cast<unsigned>(entryCount_),
                static_cast<unsigned>(registerFailures_));
  if (registerFailures_ != 0) {
    stop();
    return false;
  }
  return true;
}

void HttpdServer::stop() {
  if (handle_ != nullptr) {
    httpd_ssl_stop(handle_);
    handle_ = nullptr;
  }
}
