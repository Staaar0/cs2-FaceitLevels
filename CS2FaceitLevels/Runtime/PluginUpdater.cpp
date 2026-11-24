#include "Runtime/PluginUpdater.h"

#include "Runtime/Common.h"
#include "Runtime/HttpClient.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cfl {
namespace {

constexpr const char *kLatestRelease = "https://api.github.com/repos/Staaar0/cs2-FaceitLevels/releases/latest";
constexpr std::string_view kDownloadHost = "https://github.com/";
constexpr const char *kPackage = "CS2FaceitLevels.zip";
constexpr size_t kMaxBytes = 32 * 1024 * 1024;
#ifdef _WIN32
constexpr const char *kBinary = "CS2FaceitLevels.dll";
constexpr std::string_view kMagic = "MZ";
#else
constexpr const char *kBinary = "CS2FaceitLevels.so";
constexpr std::string_view kMagic = "\x7f" "ELF";
#endif

bool g_installed = false;

void Need(bool ok)
{
	if (!ok)
		throw std::runtime_error("bad zip");
}

uint32_t Read(std::string_view data, size_t at, size_t bytes)
{
	Need(at <= data.size() && data.size() - at >= bytes);
	uint32_t value = 0;
	for (size_t i = 0; i < bytes; ++i)
		value |= static_cast<uint32_t>(static_cast<uint8_t>(data[at + i])) << (8 * i);
	return value;
}

struct Bits
{
	std::string_view data;
	size_t at = 0;
	uint32_t held = 0;
	int count = 0;

	uint32_t Take(int bits)
	{
		for (; count < bits; count += 8)
		{
			Need(at < data.size());
			held |= static_cast<uint32_t>(static_cast<uint8_t>(data[at++])) << count;
		}
		const uint32_t value = held & ((1u << bits) - 1);
		held >>= bits;
		count -= bits;
		return value;
	}
};

struct Huffman
{
	uint16_t count[16] = {};
	uint16_t symbol[288] = {};

	Huffman(const uint8_t *lengths, int size)
	{
		for (int i = 0; i < size; ++i)
			++count[lengths[i]];
		uint16_t offset[17] = {};
		int left = 1;
		for (int length = 1; length < 16; ++length)
		{
			left = left * 2 - count[length];
			Need(left >= 0);
			offset[length + 1] = static_cast<uint16_t>(offset[length] + count[length]);
		}
		for (int i = 0; i < size; ++i)
			if (lengths[i])
				symbol[offset[lengths[i]]++] = static_cast<uint16_t>(i);
	}

	int Decode(Bits &in) const
	{
		int code = 0, first = 0, index = 0;
		for (int length = 1; length < 16; ++length)
		{
			code |= static_cast<int>(in.Take(1));
			const int codes = count[length];
			if (code - codes < first)
				return symbol[index + (code - first)];
			index += codes;
			first = (first + codes) * 2;
			code *= 2;
		}
		Need(false);
		return 0;
	}
};

void Inflate(std::string_view packed, std::string &out)
{
	static const uint16_t lengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
		131, 163, 195, 227, 258};
	static const uint8_t lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	static const uint16_t distanceBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
		1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	static const uint8_t distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
		13, 13};
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

	Bits in{packed};
	size_t at = 0;
	for (bool last = false; !last;)
	{
		last = in.Take(1) != 0;
		const uint32_t type = in.Take(2);
		if (type == 0)
		{
			in.held = 0;
			in.count = 0;
			const uint32_t size = Read(packed, in.at, 2);
			Need((Read(packed, in.at + 2, 2) ^ 0xFFFF) == size && size <= packed.size() - in.at - 4 && size <= out.size() - at);
			std::memcpy(out.data() + at, packed.data() + in.at + 4, size);
			in.at += 4 + size;
			at += size;
			continue;
		}
		Need(type != 3);
		uint8_t lengths[320] = {};
		int literals = 288, distances = 30;
		if (type == 1)
		{
			for (int i = 0; i < 288; ++i)
				lengths[i] = static_cast<uint8_t>(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
			std::memset(lengths + 288, 5, 30);
		}
		else
		{
			literals = static_cast<int>(in.Take(5)) + 257;
			distances = static_cast<int>(in.Take(5)) + 1;
			const int codes = static_cast<int>(in.Take(4)) + 4;
			Need(literals <= 286 && distances <= 30);
			for (int i = 0; i < codes; ++i)
				lengths[order[i]] = static_cast<uint8_t>(in.Take(3));
			const Huffman meta(lengths, 19);
			for (int i = 0; i < literals + distances;)
			{
				int value = meta.Decode(in), repeat = 1;
				if (value == 16)
				{
					Need(i > 0);
					value = lengths[i - 1];
					repeat = 3 + static_cast<int>(in.Take(2));
				}
				else if (value > 16)
				{
					repeat = value == 17 ? 3 + static_cast<int>(in.Take(3)) : 11 + static_cast<int>(in.Take(7));
					value = 0;
				}
				Need(repeat <= literals + distances - i);
				for (; repeat; --repeat)
					lengths[i++] = static_cast<uint8_t>(value);
			}
			Need(lengths[256] != 0);
		}
		const Huffman literal(lengths, literals), distance(lengths + literals, distances);
		for (;;)
		{
			int symbol = literal.Decode(in);
			if (symbol == 256)
				break;
			if (symbol < 256)
			{
				Need(at < out.size());
				out[at++] = static_cast<char>(symbol);
				continue;
			}
			symbol -= 257;
			Need(symbol < 29);
			size_t length = lengthBase[symbol] + in.Take(lengthExtra[symbol]);
			const int slot = distance.Decode(in);
			const size_t back = distanceBase[slot] + in.Take(distanceExtra[slot]);
			Need(back <= at && length <= out.size() - at);
			for (; length; --length, ++at)
				out[at] = out[at - back];
		}
	}
	Need(at == out.size());
}

uint32_t Checksum(std::string_view data)
{
	uint32_t table[256];
	for (uint32_t i = 0; i < 256; ++i)
	{
		table[i] = i;
		for (int bit = 0; bit < 8; ++bit)
			table[i] = (table[i] >> 1) ^ (table[i] & 1 ? 0xEDB88320u : 0u);
	}
	uint32_t crc = 0xFFFFFFFFu;
	for (const char c : data)
		crc = table[(crc ^ static_cast<uint8_t>(c)) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

bool Install(const std::string &path, const std::string &data)
{
#ifdef _WIN32
	const std::string fresh = path + ".new";
	const std::string old = path + ".old";
	if (!WriteFileAtomic(fresh, data))
		return false;
	MoveFileExA(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
	if (MoveFileExA(fresh.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
		return true;
	MoveFileExA(old.c_str(), path.c_str(), 0);
	RemoveFile(fresh);
	return false;
#else
	return WriteFileAtomic(path, data);
#endif
}

void Download(HttpClient &http, const std::string &url, const std::string &path, const std::string &version, size_t size,
	int redirects)
{
	http.Get(url, {}, 120, kMaxBytes, [&http, path, version, size, redirects](const HttpResult &result) {
		if (result.status >= 300 && result.status < 400 && redirects < 5 && result.location.rfind("https://", 0) == 0)
			return Download(http, result.location, path, version, size, redirects + 1);
		std::string binary;
		if (result.status != 200 || result.body.size() != size ||
			!Unzip(result.body, std::string("addons/CS2FaceitLevels/bin/") + kBinary, binary, kMaxBytes) ||
			binary.compare(0, kMagic.size(), kMagic) != 0 || binary.find(version + '\0') == std::string::npos)
			LogWarning("Plugin update %s failed: bad download.", version.c_str());
		else if (!Install(path, binary))
			LogWarning("Plugin update %s failed: can't replace %s, check file permissions.", version.c_str(), path.c_str());
		else
		{
			g_installed = true;
			Log("CS2FaceitLevels %s installed, restart the server to use it.", version.c_str());
		}
	});
}

}

bool Unzip(std::string_view zip, std::string_view name, std::string &out, size_t maxBytes)
{
	try
	{
		Need(zip.size() >= 22);
		size_t end = zip.size() - 22;
		for (; Read(zip, end, 4) != 0x06054b50; --end)
			Need(end > 0 && zip.size() - end < 22 + 65535);
		size_t at = Read(zip, end + 16, 4);
		for (uint32_t left = Read(zip, end + 10, 2); left; --left)
		{
			Need(Read(zip, at, 4) == 0x02014b50);
			const size_t nameSize = Read(zip, at + 28, 2);
			if (zip.substr(at + 46, nameSize) != name)
			{
				at += 46 + nameSize + Read(zip, at + 30, 2) + Read(zip, at + 32, 2);
				continue;
			}
			const uint32_t method = Read(zip, at + 10, 2), crc = Read(zip, at + 16, 4);
			const size_t packed = Read(zip, at + 20, 4), size = Read(zip, at + 24, 4), local = Read(zip, at + 42, 4);
			Need(Read(zip, local, 4) == 0x04034b50 && size <= maxBytes);
			const size_t data = local + 30 + Read(zip, local + 26, 2) + Read(zip, local + 28, 2);
			Need(data <= zip.size() && packed <= zip.size() - data);
			out.assign(size, '\0');
			if (method == 8)
				Inflate(zip.substr(data, packed), out);
			else
			{
				Need(method == 0 && packed == size);
				out.assign(zip.substr(data, packed));
			}
			Need(Checksum(out) == crc);
			return true;
		}
	}
	catch (const std::exception &)
	{
	}
	out.clear();
	return false;
}

bool IsNewerVersion(const std::string &candidate, const std::string &current)
{
	int a[3] = {}, b[3] = {};
	return std::sscanf(candidate.c_str(), "%d.%d.%d", &a[0], &a[1], &a[2]) == 3 &&
		std::sscanf(current.c_str(), "%d.%d.%d", &b[0], &b[1], &b[2]) == 3 &&
		std::lexicographical_compare(b, b + 3, a, a + 3);
}

bool UpdatePlugin(HttpClient &http, const std::string &directory, const std::string &current)
{
	const std::string path = JoinPath(directory, std::string("bin/") + kBinary);
	if (g_installed || !FileExists(path))
		return false;
	RemoveFile(path + ".old");
	return http.Get(kLatestRelease, {{"Accept", "application/vnd.github+json"}}, 10, 1024 * 1024,
		[&http, path, current](const HttpResult &result) {
			try
			{
				const nlohmann::json release = nlohmann::json::parse(result.body);
				const std::string tag = release.at("tag_name").get<std::string>();
				if (release.value("draft", false) || release.value("prerelease", false) || !IsNewerVersion(tag, current))
					return;
				const std::string version = tag.substr(0, tag.find('-'));
				for (const nlohmann::json &asset : release.at("assets"))
				{
					const std::string url = asset.at("browser_download_url").get<std::string>();
					if (asset.at("name").get<std::string>() != kPackage || url.rfind(kDownloadHost, 0) != 0)
						continue;
					Log("Downloading CS2FaceitLevels %s.", version.c_str());
					Download(http, url, path, version, asset.at("size").get<size_t>(), 0);
					return;
				}
				LogWarning("Release %s has no %s, update manually.", tag.c_str(), kPackage);
			}
			catch (const std::exception &)
			{
			}
		});
}

}
