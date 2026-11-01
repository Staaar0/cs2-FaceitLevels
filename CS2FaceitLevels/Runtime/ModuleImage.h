#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cfl {

extern void *(*g_detourAwareScan)(void *start, size_t size, const char *signature);

struct MemoryRange
{
	uintptr_t start = 0;
	size_t size = 0;
};

class ModuleImage
{
public:
	bool Open(const char *fileName, std::string &error);

	const std::string &Path() const { return path_; }
	bool Contains(const void *address) const;

	void *FindSignature(std::string_view signature) const;
	void *FindVirtualTable(std::string_view className) const;

private:
	const MemoryRange *Section(std::string_view name) const;

	std::string path_;
	uintptr_t base_ = 0;
	std::vector<MemoryRange> executable_;
	std::vector<std::pair<std::string, MemoryRange>> sections_;
};

}
