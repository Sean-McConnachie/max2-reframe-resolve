// Deflate and zlib decoding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Raw deflate stream. Appends to `out`; `used` gets the number of input bytes read.
bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t* used = nullptr);
// zlib stream (2-byte header, deflate data, Adler-32), with the checksum checked.
bool inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
