// SPDX-License-Identifier: MIT

#include "ipashelf/http.hpp"

namespace ipashelf {

void HttpHeaders::set(std::string name, std::string value) {
  for (auto& item : items_) {
    if (iequals(item.first, name)) {
      item.second = std::move(value);
      return;
    }
  }
  items_.emplace_back(std::move(name), std::move(value));
}

void HttpHeaders::add(std::string name, std::string value) {
  items_.emplace_back(std::move(name), std::move(value));
}

std::optional<std::string> HttpHeaders::get(std::string_view name) const {
  for (const auto& item : items_) {
    if (iequals(item.first, name)) return item.second;
  }
  return std::nullopt;
}

std::string HttpHeaders::get_or(std::string_view name,
                                std::string fallback) const {
  auto value = get(name);
  return value ? std::move(*value) : std::move(fallback);
}

void HttpHeaders::remove(std::string_view name) {
  for (auto iterator = items_.begin(); iterator != items_.end();) {
    if (iequals(iterator->first, name)) {
      iterator = items_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

bool HttpHeaders::contains(std::string_view name) const {
  return get(name).has_value();
}

}  // namespace ipashelf
