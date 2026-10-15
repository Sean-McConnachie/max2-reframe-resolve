// Resolve's binary fields ("...BA" elements): a small header around protocol buffer data, often zstd compressed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// "00000002" + 32-bit length + flag byte (0x80 plain, 0x81 zstd) + data, as hex text. An empty text is empty data.
bool decodeResolveBlob(const std::string& hex, std::vector<uint8_t>& data, std::string* err);
// Writes the plain (0x80) form.
std::string encodeResolveBlob(const std::vector<uint8_t>& data);

// Protocol buffer wire format.
struct PbField
{
    uint32_t num = 0;
    int wire = 0;              // 0 varint, 1 64-bit, 2 bytes, 5 32-bit
    uint64_t value = 0;        // varint, 64-bit and 32-bit values
    std::vector<uint8_t> bytes;
};
bool parsePb(const uint8_t* data, size_t size, std::vector<PbField>& out);

class PbWriter
{
public:
    PbWriter& varint(uint32_t num, uint64_t v);
    PbWriter& dbl(uint32_t num, double v);
    PbWriter& bytes(uint32_t num, const std::vector<uint8_t>& v);
    PbWriter& str(uint32_t num, const std::string& v);
    PbWriter& msg(uint32_t num, const PbWriter& m) { return bytes(num, m.data()); }
    const std::vector<uint8_t>& data() const { return m_Data; }

private:
    void key(uint32_t num, int wire) { raw(uint64_t(num) << 3 | uint64_t(wire)); }
    void raw(uint64_t v);
    std::vector<uint8_t> m_Data;
};
