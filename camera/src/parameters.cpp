#include "parameters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace wave {

namespace {

bool parseSize(const char* s, Size& out, const char** end = nullptr)
{
    char* e;
    long w = strtol(s, &e, 10);
    if (e == s || *e != 'x')
        return false;
    const char* h0 = e + 1;
    long h = strtol(h0, &e, 10);
    if (e == h0)
        return false;
    out = {static_cast<int>(w), static_cast<int>(h)};
    if (end)
        *end = e;
    return true;
}

}

void Parameters::unflatten(std::string_view flat)
{
    entries_.clear();
    while (!flat.empty()) {
        size_t semi = flat.find(';');
        std::string_view pair = flat.substr(0, semi);
        size_t eq = pair.find('=');
        if (eq != std::string_view::npos)
            store(pair.substr(0, eq), pair.substr(eq + 1));
        if (semi == std::string_view::npos)
            break;
        flat.remove_prefix(semi + 1);
    }
}

std::string Parameters::flatten() const
{
    std::string out;
    for (const auto& [k, v] : entries_) {
        if (!out.empty())
            out += ';';
        out += k;
        out += '=';
        out += v;
    }
    return out;
}

void Parameters::set(std::string_view key, std::string_view value)
{
    if (key.find_first_of("=;") != std::string_view::npos ||
        value.find_first_of("=;") != std::string_view::npos)
        return;
    store(key, value);
}

void Parameters::store(std::string_view key, std::string_view value)
{
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const auto& e) { return e.first == key; });
    if (it != entries_.end())
        it->second = value;
    else
        entries_.emplace_back(key, value);
}

void Parameters::set(std::string_view key, int value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    set(key, buf);
}

void Parameters::remove(std::string_view key)
{
    std::erase_if(entries_, [&](const auto& e) { return e.first == key; });
}

const char* Parameters::get(std::string_view key) const
{
    for (const auto& [k, v] : entries_)
        if (k == key)
            return v.c_str();
    return nullptr;
}

int Parameters::getInt(std::string_view key) const
{
    const char* v = get(key);
    if (!v)
        return -1;
    char* end;
    long n = strtol(v, &end, 10);
    if (end == v)
        return -1;
    return static_cast<int>(n);
}

Size Parameters::getSize(std::string_view key) const
{
    Size s{-1, -1};
    if (const char* v = get(key); !v || !parseSize(v, s))
        s = {-1, -1};
    return s;
}

void Parameters::setSize(std::string_view key, Size s)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%dx%d", s.width, s.height);
    set(key, buf);
}

void Parameters::previewFpsRange(int& min, int& max) const
{
    min = max = -1;
    const char* v = get("preview-fps-range");
    if (!v)
        return;
    char* end;
    long lo = strtol(v, &end, 10);
    if (end == v || *end != ',')
        return;
    const char* hi0 = end + 1;
    long hi = strtol(hi0, &end, 10);
    if (end == hi0)
        return;
    min = static_cast<int>(lo);
    max = static_cast<int>(hi);
}

std::vector<Size> Parameters::sizes(std::string_view key) const
{
    std::vector<Size> out;
    const char* p = get(key);
    while (p && *p) {
        Size s;
        const char* end;
        if (!parseSize(p, s, &end))
            break;
        out.push_back(s);
        if (*end != ',')
            break;
        p = end + 1;
    }
    return out;
}

bool listContains(const char* list, const char* value)
{
    if (!list || !value)
        return false;
    std::string_view rest(list), v(value);
    while (true) {
        size_t comma = rest.find(',');
        if (rest.substr(0, comma) == v)
            return true;
        if (comma == std::string_view::npos)
            return false;
        rest.remove_prefix(comma + 1);
    }
}

}
