#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
#include <curl/curl.h>

#include "utils.hpp"

namespace hc {

using json = nlohmann::json;

inline size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

inline json http_json(const std::string& method,
                      const std::string& url,
                      const std::string& api_key,
                      const json* body = nullptr,
                      long timeout_sec = 10) {
  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("curl_easy_init failed");

  std::string resp;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, ("X-API-Key: " + api_key).c_str());
  headers = curl_slist_append(headers, "User-Agent: HilmarCorp-Agent/1.0");

  std::string body_str;
  if (body) {
    body_str = body->dump();
    headers = curl_slist_append(headers, "Content-Type: application/json");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  } else if (method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  }

  CURLcode res = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));
  }
  if (status >= 400) {
    throw std::runtime_error("http " + std::to_string(status) + " body=" + resp);
  }
  if (resp.empty()) return json::object();
  return json::parse(resp);
}

} // namespace hc