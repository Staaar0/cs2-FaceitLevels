#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace cfl {
class HttpClient;
}

namespace cfl::workshop {

constexpr const char *kSignatureSourceUrl =
	"https://raw.githubusercontent.com/Staaar0/cs2-FaceitLevels/refs/heads/main/CS2FaceitLevels/workshop.gamedata.json";

bool UpdateSignatures(HttpClient &http, const std::string &directory, std::function<void(bool)> done);

bool ApplySignatureFile(const std::string &target, std::string_view downloaded);

}
