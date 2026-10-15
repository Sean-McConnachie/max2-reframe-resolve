// Reads the Fusion compositions that DaVinci Resolve stores in timeline files.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// A value of the Lua-like text that Fusion saves, e.g. `Input { Value = 45, }` or `{ [12] = { 1.5, RH = { 13, 2 } } }`.
struct LuaValue
{
    enum Kind { Nil, Bool, Number, String, Table } kind = Nil;
    double num = 0;
    bool b = false;
    std::string str;
    std::string ctor;  // name before a table, e.g. "Input", "BezierSpline", "ofx.com.gopro.plugins.reframe"
    std::vector<std::pair<LuaValue, LuaValue>> items;  // positional items get number keys 1, 2, ...

    const LuaValue* get(const std::string& key) const;
    const LuaValue* at(double index) const;
    bool isNumber() const { return kind == Number; }
};

bool parseLua(const std::string& text, LuaValue& out, std::string* err);

// Decodes a CompositionBA blob (hex text) into the composition's tool table. Fails if the blob has more than
// one composition.
bool decodeComposition(const std::string& hex, LuaValue& tools, std::string* err);

// A Fusion BezierSpline: value at a (fractional) frame. Before the first and after the last keyframe the value is
// constant.
class Spline
{
public:
    bool load(const LuaValue& bezierSpline, std::string* err);
    double eval(double t) const;
    bool empty() const { return m_Keys.empty(); }
    double firstTime() const { return m_Keys.front().t; }
    double lastTime() const { return m_Keys.back().t; }

private:
    struct Key
    {
        double t, v;
        bool hasLH = false, hasRH = false;
        double lht = 0, lhv = 0, rht = 0, rhv = 0;
        bool stepIn = false, stepOut = false;
    };
    std::vector<Key> m_Keys;
};
