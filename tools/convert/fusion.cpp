#include "fusion.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "inflate.h"
#include "util.h"

const LuaValue* LuaValue::get(const std::string& key) const
{
    for (const auto& it : items)
        if (it.first.kind == String && it.first.str == key) return &it.second;
    return nullptr;
}

const LuaValue* LuaValue::at(double index) const
{
    for (const auto& it : items)
        if (it.first.kind == Number && it.first.num == index) return &it.second;
    return nullptr;
}

namespace {

class Parser
{
public:
    Parser(const std::string& s) : m_S(s) {}

    bool value(LuaValue& v, int depth = 0)
    {
        if (depth > 200) return fail("nested too deeply");
        skip();
        if (m_I >= m_S.size()) return fail("unexpected end");
        char c = m_S[m_I];
        if (c == '{') return table(v, depth);
        if (c == '"' || c == '\'') return string(v);
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) return number(v);
        if (isIdentStart(c))
        {
            std::string name = ident();
            if (name == "true" || name == "false")
            {
                v.kind = LuaValue::Bool;
                v.b = name == "true";
                return true;
            }
            if (name == "nil")
            {
                v.kind = LuaValue::Nil;
                return true;
            }
            // constructor: Name { ... }, a.b.c { ... }, ordered() { ... }, or a bare name
            while (m_I < m_S.size() && m_S[m_I] == '.')
            {
                ++m_I;
                name += "." + ident();
            }
            skip();
            if (m_I + 1 < m_S.size() && m_S[m_I] == '(' && m_S[m_I + 1] == ')')
            {
                m_I += 2;
                name += "()";
                skip();
            }
            if (m_I < m_S.size() && m_S[m_I] == '{')
            {
                if (!table(v, depth)) return false;
                v.ctor = name;
                return true;
            }
            v.kind = LuaValue::String;  // a bare identifier (rare); keep its name
            v.str = name;
            return true;
        }
        return fail("unexpected character");
    }

    bool atEnd()
    {
        skip();
        return m_I >= m_S.size();
    }
    std::string error() const { return m_Err + " at offset " + std::to_string(m_I); }

private:
    const std::string& m_S;
    size_t m_I = 0;
    std::string m_Err;

    bool fail(const char* m)
    {
        if (m_Err.empty()) m_Err = m;
        return false;
    }
    static bool isIdentStart(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    static bool isIdent(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

    void skip()
    {
        for (;;)
        {
            while (m_I < m_S.size() && (unsigned char)m_S[m_I] <= ' ') ++m_I;
            if (m_I + 1 < m_S.size() && m_S[m_I] == '-' && m_S[m_I + 1] == '-')
            {
                while (m_I < m_S.size() && m_S[m_I] != '\n') ++m_I;
                continue;
            }
            return;
        }
    }

    std::string ident()
    {
        size_t b = m_I;
        while (m_I < m_S.size() && isIdent(m_S[m_I])) ++m_I;
        return m_S.substr(b, m_I - b);
    }

    bool number(LuaValue& v)
    {
        const char* b = m_S.c_str() + m_I;
        char* e = nullptr;
        double d = strtod(b, &e);
        if (e == b) return fail("bad number");
        m_I += size_t(e - b);
        v.kind = LuaValue::Number;
        v.num = d;
        return true;
    }

    bool string(LuaValue& v)
    {
        char q = m_S[m_I++];
        std::string out;
        while (m_I < m_S.size() && m_S[m_I] != q)
        {
            char c = m_S[m_I++];
            if (c == '\\' && m_I < m_S.size())
            {
                char e = m_S[m_I++];
                switch (e)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                default:
                    if (e >= '0' && e <= '9')
                    {
                        int n = e - '0';
                        for (int k = 0; k < 2 && m_I < m_S.size() && m_S[m_I] >= '0' && m_S[m_I] <= '9'; ++k) n = n * 10 + (m_S[m_I++] - '0');
                        out += char(n);
                    }
                    else out += e;
                }
            }
            else out += c;
        }
        if (m_I >= m_S.size()) return fail("unterminated string");
        ++m_I;
        v.kind = LuaValue::String;
        v.str = std::move(out);
        return true;
    }

    bool table(LuaValue& v, int depth)
    {
        ++m_I;  // '{'
        v.kind = LuaValue::Table;
        double next = 1;
        for (;;)
        {
            skip();
            if (m_I >= m_S.size()) return fail("unterminated table");
            if (m_S[m_I] == '}')
            {
                ++m_I;
                return true;
            }
            LuaValue key, val;
            if (m_S[m_I] == '[')
            {
                ++m_I;
                if (!value(key, depth + 1)) return false;
                skip();
                if (m_I >= m_S.size() || m_S[m_I] != ']') return fail("expected ]");
                ++m_I;
                skip();
                if (m_I >= m_S.size() || m_S[m_I] != '=') return fail("expected =");
                ++m_I;
                if (!value(val, depth + 1)) return false;
            }
            else
            {
                size_t save = m_I;
                bool named = false;
                if (isIdentStart(m_S[m_I]))
                {
                    std::string name = ident();
                    skip();
                    if (m_I < m_S.size() && m_S[m_I] == '=' && (m_I + 1 >= m_S.size() || m_S[m_I + 1] != '='))
                    {
                        ++m_I;
                        key.kind = LuaValue::String;
                        key.str = name;
                        named = true;
                        if (!value(val, depth + 1)) return false;
                    }
                }
                if (!named)
                {
                    m_I = save;
                    if (!value(val, depth + 1)) return false;
                    key.kind = LuaValue::Number;
                    key.num = next++;
                }
            }
            v.items.emplace_back(std::move(key), std::move(val));
            skip();
            if (m_I < m_S.size() && (m_S[m_I] == ',' || m_S[m_I] == ';')) ++m_I;
        }
    }
};

// Resolve's QVariant-like key/value list, as used inside CompositionBA.
struct Fields
{
    std::vector<std::pair<std::string, std::vector<uint8_t>>> bytes;  // byte-array values only
    int count = -1;
};

bool readFields(const std::vector<uint8_t>& b, Fields& out, std::string* err)
{
    size_t i = 0;
    auto need = [&](size_t n) { return i + n <= b.size(); };
    if (!need(8) || be32(&b[0]) != 1)
    {
        if (err) *err = "unknown composition format";
        return false;
    }
    uint32_t n = be32(&b[4]);
    i = 8;
    for (uint32_t k = 0; k < n; ++k)
    {
        if (!need(4)) return false;
        uint32_t nameLen = be32(&b[i]);
        i += 4;
        if (!need(nameLen)) return false;
        std::string name;
        for (uint32_t c = 0; c + 1 < nameLen; c += 2) name += char(b[i + c + 1]);  // UTF-16BE, ASCII names
        i += nameLen;
        if (!need(5)) return false;
        uint32_t type = be32(&b[i]);
        i += 5;  // type and the "null" flag
        switch (type)
        {
        case 1: i += 1; break;
        case 2:
        case 3:
            if (!need(4)) return false;
            if (name == "count") out.count = int(be32(&b[i]));
            i += 4;
            break;
        case 4:
        case 5:
        case 6: i += 8; break;
        case 10:
        {
            if (!need(4)) return false;
            uint32_t len = be32(&b[i]);
            i += 4;
            if (len != 0xffffffffu) i += len;
            break;
        }
        case 11:
        {
            if (!need(4)) return false;
            uint32_t cnt = be32(&b[i]);
            i += 4;
            for (uint32_t s = 0; s < cnt; ++s)
            {
                if (!need(4)) return false;
                uint32_t len = be32(&b[i]);
                i += 4;
                if (len != 0xffffffffu) i += len;
            }
            break;
        }
        case 12:
        {
            if (!need(4)) return false;
            uint32_t len = be32(&b[i]);
            i += 4;
            if (len == 0xffffffffu) len = 0;
            if (!need(len)) return false;
            out.bytes.emplace_back(name, std::vector<uint8_t>(b.begin() + i, b.begin() + i + len));
            i += len;
            break;
        }
        default:
            if (err) *err = "unknown composition field type " + std::to_string(type);
            return false;
        }
        if (i > b.size()) return false;
    }
    return true;
}

} // namespace

bool parseLua(const std::string& text, LuaValue& out, std::string* err)
{
    Parser p(text);
    if (!p.value(out))
    {
        if (err) *err = p.error();
        return false;
    }
    return true;
}

bool decodeComposition(const std::string& hex, LuaValue& tools, std::string* err)
{
    auto fail = [&](const std::string& m) {
        if (err && err->empty()) *err = m;
        return false;
    };
    std::vector<uint8_t> raw, payload;
    if (!fromHex(hex, raw) || raw.size() < 6) return fail("the composition data is damaged");
    if (!inflateZlib(raw.data() + 4, raw.size() - 4, payload)) return fail("the composition data cannot be decompressed");
    Fields f;
    if (!readFields(payload, f, err)) return fail("the composition data is damaged");
    if (f.count != 1) return fail("the clip has " + std::to_string(f.count) + " Fusion compositions");
    const std::vector<uint8_t>* data = nullptr;
    for (const auto& b : f.bytes)
        if (b.first == "0_data") data = &b.second;
    if (!data) return fail("the composition has no data");

    // "TComposition { ... Compressed = true, }" then a zero byte, the tool text size (32-bit little-endian) and
    // the tool text as a zlib stream. Without "Compressed", the tools are in the text as "Tools = ...".
    std::string text(data->begin(), data->end());
    size_t end = text.find('\0');
    std::string head = text.substr(0, end);
    std::string toolText;
    if (head.find("Compressed = true") != std::string::npos && end != std::string::npos && end + 5 < data->size())
    {
        std::vector<uint8_t> t;
        if (!inflateZlib(data->data() + end + 5, data->size() - end - 5, t)) return fail("the composition tools cannot be decompressed");
        toolText.assign(t.begin(), t.end());
        return parseLua(toolText, tools, err) || fail("the composition tools cannot be read");
    }
    LuaValue comp;
    if (!parseLua(head, comp, err)) return fail("the composition cannot be read");
    const LuaValue* t = comp.get("Tools");
    if (!t || t->kind != LuaValue::Table) return fail("the composition has no tools");
    tools = *t;
    return true;
}

bool Spline::load(const LuaValue& s, std::string* err)
{
    m_Keys.clear();
    const LuaValue* keys = s.get("KeyFrames");
    if (!keys || keys->kind != LuaValue::Table)
    {
        if (err) *err = "a curve has no keyframes";
        return false;
    }
    for (const auto& it : keys->items)
    {
        const LuaValue& k = it.second;
        const LuaValue* v = k.at(1);
        if (it.first.kind != LuaValue::Number || !v || !v->isNumber())
        {
            if (err) *err = "a keyframe cannot be read";
            return false;
        }
        Key key;
        key.t = it.first.num;
        key.v = v->num;
        auto handle = [&](const char* name, bool& has, double& ht, double& hv) {
            const LuaValue* h = k.get(name);
            if (h && h->at(1) && h->at(2) && h->at(1)->isNumber() && h->at(2)->isNumber())
            {
                has = true;
                ht = h->at(1)->num;
                hv = h->at(2)->num;
            }
        };
        handle("LH", key.hasLH, key.lht, key.lhv);
        handle("RH", key.hasRH, key.rht, key.rhv);
        if (const LuaValue* fl = k.get("Flags"))
        {
            auto flag = [&](const char* n) {
                const LuaValue* x = fl->get(n);
                return x && x->kind == LuaValue::Bool && x->b;
            };
            if (flag("Linear")) key.hasLH = key.hasRH = false;
            key.stepIn = flag("StepIn");
            key.stepOut = flag("StepOut");
        }
        m_Keys.push_back(key);
    }
    std::sort(m_Keys.begin(), m_Keys.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    if (m_Keys.empty())
    {
        if (err) *err = "a curve has no keyframes";
        return false;
    }
    return true;
}

double Spline::eval(double t) const
{
    if (t <= m_Keys.front().t) return m_Keys.front().v;
    if (t >= m_Keys.back().t) return m_Keys.back().v;
    size_t i = 0;
    while (i + 2 < m_Keys.size() && m_Keys[i + 1].t <= t) ++i;
    const Key& a = m_Keys[i];
    const Key& b = m_Keys[i + 1];
    if (a.stepOut) return a.v;
    if (b.stepIn) return t < b.t ? a.v : b.v;
    double dt = b.t - a.t;
    // Cubic Bezier through (a.t, a.v), RH of a, LH of b, (b.t, b.v). A missing handle lies on the straight line,
    // a third of the way along. Handle times are kept inside the segment so the curve stays a function of time.
    double x1 = a.hasRH ? std::clamp(a.rht, a.t, b.t) : a.t + dt / 3, y1 = a.hasRH ? a.rhv : a.v + (b.v - a.v) / 3;
    double x2 = b.hasLH ? std::clamp(b.lht, a.t, b.t) : b.t - dt / 3, y2 = b.hasLH ? b.lhv : b.v - (b.v - a.v) / 3;
    auto bez = [](double p0, double p1, double p2, double p3, double s) {
        double u = 1 - s;
        return u * u * u * p0 + 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s * p3;
    };
    double lo = 0, hi = 1;
    for (int k = 0; k < 60; ++k)
    {
        double m = (lo + hi) / 2;
        if (bez(a.t, x1, x2, b.t, m) < t) lo = m;
        else hi = m;
    }
    return bez(a.v, y1, y2, b.v, (lo + hi) / 2);
}
