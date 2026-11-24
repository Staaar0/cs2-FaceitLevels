#pragma once

#include <string>
#include <string_view>

namespace cfl {

class HttpClient;

bool UpdatePlugin(HttpClient &http, const std::string &directory, const std::string &current);

bool IsNewerVersion(const std::string &candidate, const std::string &current);

bool Unzip(std::string_view zip, std::string_view name, std::string &out, size_t maxBytes);

}
