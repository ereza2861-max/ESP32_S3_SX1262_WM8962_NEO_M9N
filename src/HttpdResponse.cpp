#include "HttpdResponse.h"

#include <cstring>
#include <cstdlib>

static const char* statusText(int code) {
  switch (code) {
    case 200: return "200, OK";
    case 201: return "201 Created";
    case 202: return "202 Accepted";
    case 204: return "204 No Content";
    case 303: return "303 See Other";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 429: return "429 Too Many Requests";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default:  return nullptr;
  }
}

void HttpdResponse::setHeader(const char* name, const char* value) {
  if (req_ == nullptr || name == nullptr || value == nullptr) return;
  httpd_resp_set_hdr(req_, name, value);
}

void HttpdResponse::setHeader(const char* name, const String& value) {
  setHeader(name, value.c_str());
}

void HttpdResponse::setType(const char* mime) {
  if (req_ == nullptr || mime == nullptr) return;
  httpd_resp_set_type(req_, mime);
}

void HttpdResponse::send(int code, const char* mime, const String& body) {
  send(code, mime, body.c_str());
}

void HttpdResponse::send(int code, const char* mime, const char* body) {
  if (req_ == nullptr) return;
  if (responded_) {
    if (!warnedDouble_) {
      Serial.println("HttpdResponse: second send() ignored");
      warnedDouble_ = true;
    }
    return;
  }
  responded_ = true;
  const char* status = statusText(code);
  if (status) httpd_resp_set_status(req_, status);
  if (mime) httpd_resp_set_type(req_, mime);
  if (body == nullptr) {
    httpd_resp_send(req_, nullptr, 0);
  } else {
    httpd_resp_sendstr(req_, body);
  }
}

void HttpdResponse::sendEmpty(int code) {
  if (req_ == nullptr || responded_) return;
  responded_ = true;
  const char* status = statusText(code);
  if (status) httpd_resp_set_status(req_, status);
  httpd_resp_send(req_, nullptr, 0);
}

void HttpdResponse::sendJson(int code, const String& body) {
  send(code, "application/json", body);
}

void HttpdResponse::sendHtml(int code, const String& body) {
  send(code, "text/html", body);
}

void HttpdResponse::sendText(int code, const String& body) {
  send(code, "text/plain", body);
}

void HttpdResponse::sendCsv(int code, const String& body) {
  send(code, "text/csv", body);
}

void HttpdResponse::send401Basic(const char* realm, const char* message) {
  const char* r = realm ? realm : "Login Required";
  String authHeader = String("Basic realm=\"") + r + "\"";
  setHeader("WWW-Authenticate", authHeader);
  send(401, "text/plain", message ? message : "");
}

void HttpdResponse::send403(const char* message) {
  send(403, "text/plain", message ? message : "forbidden");
}

void HttpdResponse::send404(const char* message) {
  send(404, "text/plain", message ? message : "not found");
}

void HttpdResponse::send413(const char* message) {
  send(413, "text/plain", message ? message : "payload too large");
}

void HttpdResponse::send429(const char* message) {
  send(429, "text/plain", message ? message : "rate limited");
}

void HttpdResponse::send500(const char* message) {
  send(500, "text/plain", message ? message : "internal error");
}

void HttpdResponse::send503(const char* message) {
  send(503, "text/plain", message ? message : "busy");
}

bool HttpdResponse::streamFile(File& file, const char* mime) {
  if (req_ == nullptr || responded_) return false;
  if (!file) return false;
  responded_ = true;

  httpd_resp_set_status(req_, "200 OK");
  if (mime) httpd_resp_set_type(req_, mime);
  // Content-Length is intentionally NOT set: httpd_resp_send_chunk() below
  // uses HTTP chunked transfer encoding, and the two are mutually exclusive.

  constexpr size_t CHUNK = 4096;
  uint8_t* buf = static_cast<uint8_t*>(malloc(CHUNK));
  if (buf == nullptr) {
    httpd_resp_send_err(req_, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    return false;
  }
  esp_err_t err = ESP_OK;
  while (file.available()) {
    size_t read = file.read(buf, CHUNK);
    if (read == 0) break;
    err = httpd_resp_send_chunk(req_, reinterpret_cast<const char*>(buf), read);
    if (err != ESP_OK) break;
  }
  free(buf);
  // Terminate the chunked response.
  if (err == ESP_OK) {
    err = httpd_resp_send_chunk(req_, nullptr, 0);
  }
  if (err != ESP_OK) {
    Serial.printf("HttpdResponse::streamFile: %s\n", esp_err_to_name(err));
    return false;
  }
  return true;
}
