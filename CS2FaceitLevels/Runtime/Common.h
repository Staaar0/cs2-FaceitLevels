#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define CFL_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define CFL_PRINTF(fmt, args)
#endif

namespace cfl {

double Now();

int64_t UnixNow();

using LogSink = void (*)(const char *line);
void SetLogSink(LogSink sink);
void MarkGameThread();
bool OnGameThread();
void Log(const char *fmt, ...) CFL_PRINTF(1, 2);
void LogWarning(const char *fmt, ...) CFL_PRINTF(1, 2);
void LogError(const char *fmt, ...) CFL_PRINTF(1, 2);

std::vector<std::string_view> SplitNonEmpty(std::string_view text, char separator);
std::string_view Trim(std::string_view text);
std::string Join(const std::vector<std::string> &items, char separator);
bool IsDigits(std::string_view text);
bool ParseUint64(std::string_view text, uint64_t &value);

bool ReadFile(const std::string &path, std::string &out, size_t maxBytes);
bool WriteFileAtomic(const std::string &path, std::string_view data);
bool FileExists(const std::string &path);
bool RemoveFile(const std::string &path);
std::string JoinPath(const std::string &directory, const std::string &name);

}
