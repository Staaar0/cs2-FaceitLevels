#pragma once

#include <string>
#include <string_view>

namespace cfl::chat {

std::string MissingName();
std::string NoMatch(std::string_view search);
std::string Multiple(std::string_view names);
std::string EloLine(std::string_view playerName, int level, int elo);

std::string Literal(std::string_view text);
std::string FoldCase(std::string_view text);

}
