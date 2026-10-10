#include "HttpdRequest.h"

#include <cstring>
#include <cstdlib>
#include <new>
#include <lwip/sockets.h>
#include <lwip/inet.h>

String HttpdRequest::header(const char* name) const {
  if (req_ == nullptr || name == nullptr) return String();
  size_t len = httpd_req_get_hdr_value_len(req_, name);
  if (len == 0) return String();
  // ESP-IDF returns length excluding NUL. Allocate len+1.
  std::unique_ptr<char[]> buf(new (std::nothrow) char[len + 1]);
  if (!buf) return String();
  if (httpd_req_get_hdr_value_str(req_, name, buf.get(), len + 1) != ESP_OK) {
    return String();
  }
  buf[len] = '\0';
  return String(buf.get());
}

String HttpdRequest::query(const char* name) const {
  if (req_ == nullptr || name == nullptr) return String();
  size_t queryLen = httpd_req_get_url_query_len(req_);
  if (queryLen == 0) return String();
  std::unique_ptr<char[]> buf(new (std::nothrow) char[queryLen + 1]);
  if (!buf) return String();
  if (httpd_req_get_url_query_str(req_, buf.get(), queryLen + 1) != ESP_OK) {
    return String();
  }
  // httpd_query_key_value expects the raw query buffer (without leading '?')
  // and does URL-decoding of the value into val.
  char value[512] = {};
  if (httpd_query_key_value(buf.get(), name, value, sizeof(value)) != ESP_OK) {
    return String();
  }
  return String(value);
}

bool HttpdRequest::hasQuery(const char* name) const {
  if (req_ == nullptr || name == nullptr) return false;
  size_t queryLen = httpd_req_get_url_query_len(req_);
  if (queryLen == 0) return false;
  std::unique_ptr<char[]> buf(new (std::nothrow) char[queryLen + 1]);
  if (!buf) return false;
  if (httpd_req_get_url_query_str(req_, buf.get(), queryLen + 1) != ESP_OK) {
    return false;
  }
  char value[512] = {};
  return httpd_query_key_value(buf.get(), name, value, sizeof(value)) == ESP_OK;
}

void HttpdRequest::ensureBodyParsed() const {
  if (bodyParsed_) return;
  bodyParsed_ = true;
  if (req_ == nullptr) return;
  if (req_->content_len == 0) return;

  // Only urlencoded bodies are parsed here. multipart bodies are streamed by
  // HttpdMultipart and must not be consumed by arg().
  String contentType = header("Content-Type");
  if (contentType.indexOf("application/x-www-form-urlencoded") < 0) return;
  if (req_->content_len > 4096) {
    // Refuse to allocate unbounded input. Handlers that need large bodies
    // must call bodyText() directly and reject oversize with 413.
    bodyOverflow_ = true;
    return;
  }

  String raw;
  raw.reserve(req_->content_len);
  char buf[257];
  size_t remaining = req_->content_len;
  while (remaining > 0) {
    size_t chunk = remaining < 256 ? remaining : 256;
    int received = httpd_req_recv(req_, buf, chunk);
    if (received <= 0) {
      bodyOverflow_ = true;
      return;
    }
    buf[received] = '\0';
    raw += buf;
    remaining -= static_cast<size_t>(received);
  }

  // Simple key=value&key=value parser with URL-decoding.
  size_t pos = 0;
  while (pos <= raw.length()) {
    size_t amp = raw.indexOf('&', pos);
    if (amp < 0) amp = raw.length();
    String pair = raw.substring(pos, amp);
    int eq = pair.indexOf('=');
    if (eq > 0) {
      String key = pair.substring(0, eq);
      String value = pair.substring(eq + 1);
      // URL decode '+' -> ' ' then %XX. Minimal but sufficient for WebUI.
      value.replace("+", " ");
      String decoded;
      decoded.reserve(value.length());
      for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (c == '%' && i + 2 < value.length()) {
          auto hexv = [](char h) -> int {
            if (h >= '0' && h <= '9') return h - '0';
            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
            return -1;
          };
          int hi = hexv(value[i + 1]);
          int lo = hexv(value[i + 2]);
          if (hi >= 0 && lo >= 0) {
            decoded += static_cast<char>((hi << 4) | lo);
            i += 2;
            continue;
          }
        }
        decoded += c;
      }
      bodyFields_[key] = decoded;
    }
    if (amp >= raw.length()) break;
    pos = amp + 1;
  }
}

String HttpdRequest::arg(const char* name) const {
  if (name == nullptr) return String();
  // Query string first, matching the old compat layer's merge order.
  String q = query(name);
  if (!q.isEmpty()) return q;
  if (hasQuery(name)) return String(); // present but empty
  ensureBodyParsed();
  auto it = bodyFields_.find(String(name));
  if (it != bodyFields_.end()) return it->second;
  return String();
}

bool HttpdRequest::hasArg(const char* name) const {
  if (name == nullptr) return false;
  if (hasQuery(name)) return true;
  ensureBodyParsed();
  return bodyFields_.find(String(name)) != bodyFields_.end();
}

bool HttpdRequest::bodyText(String& out, size_t maxBytes) const {
  out = String();
  if (req_ == nullptr) return false;
  if (req_->content_len == 0) return true;
  if (req_->content_len > maxBytes) return false;

  out.reserve(req_->content_len);
  char buf[257];
  size_t remaining = req_->content_len;
  while (remaining > 0) {
    size_t chunk = remaining < 256 ? remaining : 256;
    int received = httpd_req_recv(req_, buf, chunk);
    if (received <= 0) return false;
    buf[received] = '\0';
    out += buf;
    remaining -= static_cast<size_t>(received);
  }
  return true;
}

IPAddress HttpdRequest::remoteIp() const {
  if (req_ == nullptr) return IPAddress(0, 0, 0, 0);
  int fd = httpd_req_to_sockfd(req_);
  if (fd < 0) return IPAddress(0, 0, 0, 0);
  struct sockaddr_in6 addr6 = {};
  socklen_t len = sizeof(addr6);
  if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr6), &len) != 0) {
    return IPAddress(0, 0, 0, 0);
  }
  if (addr6.sin6_family == AF_INET6) {
    // IPv4-mapped IPv6 ::ffff:a.b.c.d
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr6.sin6_addr);
    // lwIP stores IPv4-mapped as 10 zero bytes + 0xFF 0xFF + 4 bytes.
    if (b[10] == 0xFF && b[11] == 0xFF) {
      return IPAddress(b[12], b[13], b[14], b[15]);
    }
    // Otherwise fall back to a synthetic value based on last 4 bytes.
    return IPAddress(b[12], b[13], b[14], b[15]);
  }
  const struct sockaddr_in* addr4 = reinterpret_cast<const struct sockaddr_in*>(&addr6);
  uint32_t raw = addr4->sin_addr.s_addr;
  return IPAddress(static_cast<uint8_t>(raw & 0xFF),
                   static_cast<uint8_t>((raw >> 8) & 0xFF),
                   static_cast<uint8_t>((raw >> 16) & 0xFF),
                   static_cast<uint8_t>((raw >> 24) & 0xFF));
}
