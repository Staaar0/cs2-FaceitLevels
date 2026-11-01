#include "Runtime/Common.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace cfl {
namespace {

void DefaultSink(const char *line)
{
	std::fputs(line, stdout);
}

LogSink g_sink = &DefaultSink;
std::atomic<std::thread::id> g_gameThread{};

void Emit(const char *level, const char *fmt, va_list args)
{
	char message[2048];
	std::vsnprintf(message, sizeof(message), fmt, args);
	char line[2200];
	std::snprintf(line, sizeof(line), "[CS2FaceitLevels] %s%s\n", level, message);
	g_sink(line);
}

}

double Now()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int64_t UnixNow()
{
	return static_cast<int64_t>(std::time(nullptr));
}

void SetLogSink(LogSink sink)
{
	g_sink = sink ? sink : &DefaultSink;
}

void MarkGameThread()
{
	g_gameThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

bool OnGameThread()
{
	const std::thread::id game = g_gameThread.load(std::memory_order_relaxed);
	return game != std::thread::id() && game == std::this_thread::get_id();
}

void Log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	Emit("", fmt, args);
	va_end(args);
}

void LogWarning(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	Emit("Warning: ", fmt, args);
	va_end(args);
}

void LogError(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	Emit("Error: ", fmt, args);
	va_end(args);
}

std::vector<std::string_view> SplitNonEmpty(std::string_view text, char separator)
{
	std::vector<std::string_view> parts;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t end = text.find(separator, start);
		if (end == std::string_view::npos)
			end = text.size();
		if (end > start)
			parts.push_back(text.substr(start, end - start));
		start = end + 1;
	}
	return parts;
}

std::string_view Trim(std::string_view text)
{
	auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
	while (!text.empty() && space(text.front()))
		text.remove_prefix(1);
	while (!text.empty() && space(text.back()))
		text.remove_suffix(1);
	return text;
}

std::string Join(const std::vector<std::string> &items, char separator)
{
	std::string result;
	for (size_t i = 0; i < items.size(); ++i)
	{
		if (i)
			result += separator;
		result += items[i];
	}
	return result;
}

bool IsDigits(std::string_view text)
{
	if (text.empty())
		return false;
	for (char c : text)
		if (c < '0' || c > '9')
			return false;
	return true;
}

bool ParseUint64(std::string_view text, uint64_t &value)
{
	if (!IsDigits(text))
		return false;
	uint64_t result = 0;
	for (char c : text)
	{
		const uint64_t digit = static_cast<uint64_t>(c - '0');
		if (result > (UINT64_MAX - digit) / 10)
			return false;
		result = result * 10 + digit;
	}
	value = result;
	return true;
}

bool ReadFile(const std::string &path, std::string &out, size_t maxBytes)
{
	out.clear();
	FILE *file = std::fopen(path.c_str(), "rb");
	if (!file)
		return false;
	bool ok = false;
	if (std::fseek(file, 0, SEEK_END) == 0)
	{
		const long size = std::ftell(file);
		if (size >= 0 && static_cast<unsigned long>(size) <= maxBytes && std::fseek(file, 0, SEEK_SET) == 0)
		{
			out.resize(static_cast<size_t>(size));
			ok = size == 0 || std::fread(out.data(), 1, out.size(), file) == out.size();
		}
	}
	std::fclose(file);
	if (!ok)
		out.clear();
	return ok;
}

bool WriteFileAtomic(const std::string &path, std::string_view data)
{
	static std::atomic<unsigned> counter{0};
#ifdef _WIN32
	const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
	const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
	char suffix[64];
	std::snprintf(suffix, sizeof(suffix), ".%lu.%u.tmp", pid, counter.fetch_add(1));
	const std::string temporary = path + suffix;

	FILE *file = std::fopen(temporary.c_str(), "wb");
	if (!file)
		return false;
	bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();
	ok = std::fflush(file) == 0 && ok;
	ok = std::fclose(file) == 0 && ok;
	if (ok)
	{
#ifdef _WIN32
		ok = MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
		ok = std::rename(temporary.c_str(), path.c_str()) == 0;
#endif
	}
	if (!ok)
		std::remove(temporary.c_str());
	return ok;
}

bool FileExists(const std::string &path)
{
	struct stat info;
	return stat(path.c_str(), &info) == 0;
}

bool RemoveFile(const std::string &path)
{
	return std::remove(path.c_str()) == 0;
}

std::string JoinPath(const std::string &directory, const std::string &name)
{
	if (directory.empty())
		return name;
	const char last = directory.back();
	if (last == '/' || last == '\\')
		return directory + name;
	return directory + "/" + name;
}

}
