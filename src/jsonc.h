// jsonc.h — JSONC (JSON with comments) parser: string-aware comment stripping + minimal JSON DOM.
#pragma once
#include <string>
#include <vector>
#include <utility>

// Strip // line comments and /* */ block comments (outside strings). UTF-8 safe.
std::string stripJsonc(const std::string& src);

struct Json {
    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    bool isObj() const { return type == Type::Obj; }
    bool isArr() const { return type == Type::Arr; }
    const Json* get(const std::string& key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    const Json* getPath(const char* k1, const char* k2 = nullptr) const {
        const Json* j = get(k1);
        if (!j) return nullptr;
        if (k2) j = j->get(k2);
        return j;
    }
    std::string asStr(const std::string& def = "") const { return type == Type::Str ? str : def; }
    double asNum(double def = 0) const { return type == Type::Num ? num : def; }
    bool asBool(bool def = false) const { return type == Type::Bool ? b : def; }
};

// Parse JSONC text (comments allowed); on failure returns Json::Null.
Json parseJsonc(const std::string& src);