// Minimal zip archive reading and writing, for DaVinci Resolve .drt files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ZipEntry
{
    std::string name;  // UTF-8, '/' separated
    std::vector<uint8_t> data;
};

// Reads all entries (stored or deflated). Checks each CRC-32.
bool readZip(const std::wstring& path, std::vector<ZipEntry>& entries, std::string* err);
// Writes the entries uncompressed ("stored"), in the given order.
bool writeZip(const std::wstring& path, const std::vector<ZipEntry>& entries, std::string* err);

uint32_t crc32(const uint8_t* data, size_t size);
