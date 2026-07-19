#include "bedrock/_java-player.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#if defined(JE2BE_WITH_CURL)
#include <curl/curl.h>
#endif

namespace je2be::bedrock {
namespace {

constexpr std::u8string_view kMarkerPrefix = u8"JavaTag=";

bool IsJavaPlayerName(std::u8string_view name) {
  if (name.size() < 3 || name.size() > 16) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char8_t ch) {
    return (ch >= u8'A' && ch <= u8'Z') ||
           (ch >= u8'a' && ch <= u8'z') ||
           (ch >= u8'0' && ch <= u8'9') ||
           ch == u8'_';
  });
}

std::optional<std::u8string> NameFromMarker(std::u8string_view marker) {
  if (!marker.starts_with(kMarkerPrefix)) {
    return std::nullopt;
  }

  marker.remove_prefix(kMarkerPrefix.size());
  if (marker.size() >= 2 && marker.front() == u8'(' && marker.back() == u8')') {
    marker.remove_prefix(1);
    marker.remove_suffix(1);
  }
  if (!IsJavaPlayerName(marker)) {
    return std::nullopt;
  }
  return std::u8string(marker);
}

bool IsHexUuid(std::string_view value) {
  if (value.size() != 32) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char ch) {
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
  });
}

#if defined(JE2BE_WITH_CURL)

constexpr size_t kMaxMojangResponseSize = 16 * 1024;

class CurlGlobal {
public:
  CurlGlobal()
      : fInitialized(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}

  ~CurlGlobal() {
    if (fInitialized) {
      curl_global_cleanup();
    }
  }

  bool fInitialized;
};

size_t WriteMojangResponse(char *data, size_t size, size_t count, void *userdata) {
  auto response = static_cast<std::string *>(userdata);
  if (size != 0 && count > kMaxMojangResponseSize / size) {
    return 0;
  }
  size_t const bytes = size * count;
  if (bytes > kMaxMojangResponseSize - response->size()) {
    return 0;
  }
  response->append(data, bytes);
  return bytes;
}

std::optional<Uuid> ResolveUuidFromMojang(std::u8string const &name) {
  static CurlGlobal curlGlobal;
  if (!curlGlobal.fInitialized) {
    return std::nullopt;
  }

  using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
  CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
  if (!curl) {
    return std::nullopt;
  }

  std::string const nameString(reinterpret_cast<char const *>(name.data()), name.size());
  std::string const url = "https://api.mojang.com/users/profiles/minecraft/" + nameString;
  std::string response;
  response.reserve(512);

  bool const configured =
      curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 10000L) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxMojangResponseSize)) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "je2be-core") == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &WriteMojangResponse) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response) == CURLE_OK;
  if (!configured || curl_easy_perform(curl.get()) != CURLE_OK) {
    return std::nullopt;
  }

  long status = 0;
  if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200) {
    return std::nullopt;
  }
  return JavaPlayer::UuidFromMojangResponse(response);
}

#endif

} // namespace

std::optional<std::u8string> JavaPlayer::NameFromInventoryMarker(CompoundTag const &player) {
  auto inventory = player.listTag(u8"Inventory");
  if (!inventory) {
    return std::nullopt;
  }

  for (auto const &entry : *inventory) {
    auto item = entry ? entry->asCompound() : nullptr;
    if (!item) {
      continue;
    }
    if (item->byte(u8"Count", 0) <= 0) {
      continue;
    }
    auto tag = item->compoundTag(u8"tag");
    auto display = tag ? tag->compoundTag(u8"display") : nullptr;
    auto name = display ? display->string(u8"Name") : std::nullopt;
    if (!name) {
      continue;
    }
    if (auto parsed = NameFromMarker(*name); parsed) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<Uuid> JavaPlayer::UuidFromMojangResponse(std::string_view response) {
  auto json = nlohmann::json::parse(response.begin(), response.end(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return std::nullopt;
  }
  auto found = json.find("id");
  if (found == json.end() || !found->is_string()) {
    return std::nullopt;
  }

  auto const &id = found->get_ref<std::string const &>();
  if (!IsHexUuid(id)) {
    return std::nullopt;
  }
  std::u8string const uuid(reinterpret_cast<char8_t const *>(id.data()), id.size());
  return Uuid::FromString(uuid);
}

std::optional<Uuid> JavaPlayer::ResolveUuid(std::u8string const &name, Options const &options) {
  if (!IsJavaPlayerName(name)) {
    return std::nullopt;
  }
  if (options.fJavaPlayerUuidResolver) {
    return options.fJavaPlayerUuidResolver(name);
  }
#if defined(JE2BE_WITH_CURL)
  return ResolveUuidFromMojang(name);
#else
  return std::nullopt;
#endif
}

std::optional<Uuid> JavaPlayer::ResolveUuid(CompoundTag const &player, Options const &options) {
  auto name = NameFromInventoryMarker(player);
  if (!name) {
    return std::nullopt;
  }
  return ResolveUuid(*name, options);
}

} // namespace je2be::bedrock
