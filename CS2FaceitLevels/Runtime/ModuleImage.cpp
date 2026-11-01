#include "Runtime/ModuleImage.h"

#include "Runtime/Common.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cfl {

void *(*g_detourAwareScan)(void *start, size_t size, const char *signature) = nullptr;

namespace {

struct Pattern
{
	std::vector<uint8_t> bytes;
	std::vector<bool> exact;
	size_t anchor = 0;
};

bool ParsePattern(std::string_view signature, Pattern &pattern)
{
	auto hex = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (std::string_view token : SplitNonEmpty(signature, ' '))
	{
		if (token == "?" || token == "??")
		{
			pattern.bytes.push_back(0);
			pattern.exact.push_back(false);
			continue;
		}
		if (token.size() != 2 || hex(token[0]) < 0 || hex(token[1]) < 0)
			return false;
		pattern.bytes.push_back(static_cast<uint8_t>(hex(token[0]) * 16 + hex(token[1])));
		pattern.exact.push_back(true);
	}
	for (size_t i = 0; i < pattern.exact.size(); ++i)
		if (pattern.exact[i])
		{
			pattern.anchor = i;
			return true;
		}
	return false;
}

const uint8_t *Scan(const uint8_t *start, size_t size, const Pattern &pattern)
{
	const size_t length = pattern.bytes.size();
	if (size < length)
		return nullptr;
	const uint8_t first = pattern.bytes[pattern.anchor];
	const uint8_t *cursor = start + pattern.anchor;
	const uint8_t *last = start + (size - length) + pattern.anchor;
	while (cursor <= last)
	{
		const void *hit = std::memchr(cursor, first, static_cast<size_t>(last - cursor) + 1);
		if (!hit)
			return nullptr;
		const uint8_t *candidate = static_cast<const uint8_t *>(hit) - pattern.anchor;
		size_t i = 0;
		while (i < length && (!pattern.exact[i] || candidate[i] == pattern.bytes[i]))
			++i;
		if (i == length)
			return candidate;
		cursor = static_cast<const uint8_t *>(hit) + 1;
	}
	return nullptr;
}

std::vector<uintptr_t> FindStrings(const MemoryRange &range, const std::string &needle)
{
	std::vector<uintptr_t> found;
	const char *begin = reinterpret_cast<const char *>(range.start);
	const size_t size = range.size;
	const size_t length = needle.size() + 1;
	size_t offset = 0;
	while (offset + length <= size)
	{
		const void *hit = std::memchr(begin + offset, needle[0], size - offset - length + 1);
		if (!hit)
			break;
		const size_t at = static_cast<size_t>(static_cast<const char *>(hit) - begin);
		if (std::memcmp(begin + at, needle.c_str(), length) == 0)
			found.push_back(range.start + at);
		offset = at + 1;
	}
	return found;
}

template <typename T>
std::vector<uintptr_t> FindValues(const MemoryRange &range, T value)
{
	std::vector<uintptr_t> found;
	const uintptr_t first = (range.start + alignof(T) - 1) & ~static_cast<uintptr_t>(alignof(T) - 1);
	const uintptr_t end = range.start + range.size;
	for (uintptr_t at = first; at + sizeof(T) <= end; at += alignof(T))
	{
		T current;
		std::memcpy(&current, reinterpret_cast<const void *>(at), sizeof(T));
		if (current == value)
			found.push_back(at);
	}
	return found;
}

int GamePathScore(std::string path)
{
	for (char &c : path)
	{
		if (c == '\\')
			c = '/';
		else if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return path.find("/csgo/addons/") == std::string::npos ? 1 : 0;
}

#ifndef _WIN32
struct PhdrSearch
{
	const char *fileName;
	std::string path;
	uintptr_t base = 0;
	std::vector<MemoryRange> executable;
	int score = -1;
};

int PhdrCallback(dl_phdr_info *info, size_t, void *data)
{
	auto *search = static_cast<PhdrSearch *>(data);
	if (!info->dlpi_name || !*info->dlpi_name)
		return 0;
	const char *slash = std::strrchr(info->dlpi_name, '/');
	const char *name = slash ? slash + 1 : info->dlpi_name;
	if (std::strcmp(name, search->fileName) != 0)
		return 0;
	const int score = GamePathScore(info->dlpi_name);
	if (score <= search->score)
		return 0;
	search->score = score;
	search->path = info->dlpi_name;
	search->base = static_cast<uintptr_t>(info->dlpi_addr);
	search->executable.clear();
	for (int i = 0; i < info->dlpi_phnum; ++i)
	{
		const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
		if (phdr.p_type == PT_LOAD && (phdr.p_flags & PF_X))
			search->executable.push_back({search->base + phdr.p_vaddr, static_cast<size_t>(phdr.p_memsz)});
	}
	return 0;
}
#else
HMODULE FindGameModule(const char *fileName)
{
	HMODULE modules[1024];
	DWORD needed = 0;
	if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
		return GetModuleHandleA(fileName);
	const DWORD count = (std::min)(static_cast<DWORD>(needed / sizeof(HMODULE)), static_cast<DWORD>(sizeof(modules) / sizeof(HMODULE)));
	HMODULE best = nullptr;
	int bestScore = -1;
	for (DWORD i = 0; i < count; ++i)
	{
		char path[MAX_PATH] = {};
		if (!GetModuleFileNameA(modules[i], path, MAX_PATH))
			continue;
		const char *slash = std::strrchr(path, '\\');
		const char *name = slash ? slash + 1 : path;
		if (_stricmp(name, fileName) != 0)
			continue;
		const int score = GamePathScore(path);
		if (score > bestScore)
		{
			bestScore = score;
			best = modules[i];
		}
	}
	return best;
}
#endif

}

bool ModuleImage::Open(const char *fileName, std::string &error)
{
	path_.clear();
	base_ = 0;
	executable_.clear();
	sections_.clear();

#ifdef _WIN32
	HMODULE module = FindGameModule(fileName);
	if (!module)
	{
		error = std::string(fileName) + " is not loaded";
		return false;
	}
	char path[MAX_PATH] = {};
	GetModuleFileNameA(module, path, MAX_PATH);
	path_ = path;
	base_ = reinterpret_cast<uintptr_t>(module);
	auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base_);
	auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base_ + dos->e_lfanew);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE)
	{
		error = std::string(fileName) + " has an unexpected PE header";
		return false;
	}
	const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
	{
		std::string name(reinterpret_cast<const char *>(section->Name),
			strnlen(reinterpret_cast<const char *>(section->Name), IMAGE_SIZEOF_SHORT_NAME));
		const size_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
		MemoryRange range{base_ + section->VirtualAddress, size};
		sections_.emplace_back(name, range);
		if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
			executable_.push_back(range);
	}
#else
	PhdrSearch search;
	search.fileName = fileName;
	dl_iterate_phdr(&PhdrCallback, &search);
	if (search.score < 0)
	{
		error = std::string(fileName) + " is not loaded";
		return false;
	}
	path_ = search.path;
	base_ = search.base;
	executable_ = std::move(search.executable);

	const int fd = open(path_.c_str(), O_RDONLY);
	if (fd < 0)
	{
		error = "cannot open " + path_;
		return false;
	}
	struct stat info;
	void *map = MAP_FAILED;
	if (fstat(fd, &info) == 0 && info.st_size > static_cast<off_t>(sizeof(ElfW(Ehdr))))
		map = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
	{
		error = "cannot map " + path_;
		return false;
	}
	const auto *bytes = static_cast<const uint8_t *>(map);
	const size_t fileSize = static_cast<size_t>(info.st_size);
	const auto *header = reinterpret_cast<const ElfW(Ehdr) *>(bytes);
	const bool validHeader = std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
		header->e_shentsize == sizeof(ElfW(Shdr)) && header->e_shoff + static_cast<size_t>(header->e_shnum) * sizeof(ElfW(Shdr)) <= fileSize &&
		header->e_shstrndx < header->e_shnum;
	if (validHeader)
	{
		const auto *sectionHeaders = reinterpret_cast<const ElfW(Shdr) *>(bytes + header->e_shoff);
		const ElfW(Shdr) &names = sectionHeaders[header->e_shstrndx];
		for (unsigned i = 0; i < header->e_shnum; ++i)
		{
			const ElfW(Shdr) &section = sectionHeaders[i];
			if (!(section.sh_flags & SHF_ALLOC) || section.sh_name >= names.sh_size || names.sh_offset + names.sh_size > fileSize)
				continue;
			const char *name = reinterpret_cast<const char *>(bytes + names.sh_offset + section.sh_name);
			sections_.emplace_back(std::string(name, strnlen(name, names.sh_size - section.sh_name)),
				MemoryRange{base_ + section.sh_addr, static_cast<size_t>(section.sh_size)});
		}
	}
	munmap(map, fileSize);
	if (!validHeader)
	{
		error = path_ + " has an unexpected ELF header";
		return false;
	}
#endif
	if (executable_.empty())
	{
		error = path_ + " has no executable code";
		return false;
	}
	return true;
}

const MemoryRange *ModuleImage::Section(std::string_view name) const
{
	for (const auto &section : sections_)
		if (section.first == name)
			return &section.second;
	return nullptr;
}

bool ModuleImage::Contains(const void *address) const
{
	const auto value = reinterpret_cast<uintptr_t>(address);
	for (const auto &section : sections_)
		if (value >= section.second.start && value < section.second.start + section.second.size)
			return true;
	return false;
}

void *ModuleImage::FindSignature(std::string_view signature) const
{
	Pattern pattern;
	if (!ParsePattern(signature, pattern))
		return nullptr;
	for (const MemoryRange &range : executable_)
		if (const uint8_t *hit = Scan(reinterpret_cast<const uint8_t *>(range.start), range.size, pattern))
			return const_cast<uint8_t *>(hit);
	if (g_detourAwareScan)
	{
		std::string normalized;
		for (std::string_view token : SplitNonEmpty(signature, ' '))
			normalized += (normalized.empty() ? "" : " ") + std::string(token);
		for (const MemoryRange &range : executable_)
			if (void *hit = g_detourAwareScan(reinterpret_cast<void *>(range.start), range.size, normalized.c_str()))
				return hit;
	}
	return nullptr;
}

void *ModuleImage::FindVirtualTable(std::string_view className) const
{
#ifdef _WIN32
	const MemoryRange *data = Section(".data");
	const MemoryRange *rdata = Section(".rdata");
	if (!data || !rdata)
		return nullptr;
	for (const char *prefix : {".?AV", ".?AU"})
	{
		for (uintptr_t name : FindStrings(*data, prefix + std::string(className) + "@@"))
		{
			const uintptr_t descriptor = name - 0x10;
			const auto descriptorRva = static_cast<uint32_t>(descriptor - base_);
			for (uintptr_t reference : FindValues<uint32_t>(*rdata, descriptorRva))
			{
				const uintptr_t locator = reference - 0xC;
				int32_t signature = 0, offset = 0;
				std::memcpy(&signature, reinterpret_cast<const void *>(locator), 4);
				std::memcpy(&offset, reinterpret_cast<const void *>(locator + 4), 4);
				if (signature != 1 || offset != 0)
					continue;
				for (uintptr_t slot : FindValues<uintptr_t>(*rdata, locator))
					return reinterpret_cast<void *>(slot + sizeof(void *));
			}
		}
	}
	return nullptr;
#else
	const MemoryRange *rodata = Section(".rodata");
	if (!rodata)
		return nullptr;
	std::vector<const MemoryRange *> relro;
	for (const char *name : {".data.rel.ro", ".data.rel.ro.local"})
		if (const MemoryRange *range = Section(name))
			relro.push_back(range);
	if (relro.empty())
		return nullptr;

	const std::string mangled = std::to_string(className.size()) + std::string(className);
	for (uintptr_t name : FindStrings(*rodata, mangled))
	{
		for (const MemoryRange *nameSection : relro)
		{
			for (uintptr_t nameField : FindValues<uintptr_t>(*nameSection, name))
			{
				const uintptr_t typeInfo = nameField - sizeof(void *);
				for (const MemoryRange *vtableSection : relro)
				{
					for (uintptr_t slot : FindValues<uintptr_t>(*vtableSection, typeInfo))
					{
						intptr_t offsetToTop = 0;
						std::memcpy(&offsetToTop, reinterpret_cast<const void *>(slot - sizeof(void *)), sizeof(void *));
						if (slot - sizeof(void *) >= vtableSection->start && offsetToTop == 0)
							return reinterpret_cast<void *>(slot + sizeof(void *));
					}
				}
			}
		}
	}
	return nullptr;
#endif
}

}
