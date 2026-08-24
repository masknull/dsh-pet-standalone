// jsonc.cpp — JSONC parser implementation.
#include "jsonc.h"
#include <cstdlib>
#include <cstring>

std::string stripJsonc(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool inStr = false;
    bool esc = false;
    size_t i = 0;
    const size_t n = src.size();
    while (i < n) {
        char c = src[i];
        if (inStr) {
            out.push_back(c);
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                inStr = false;
            }
            i++;
            continue;
        }
        if (c == '"') {
            inStr = true;
            out.push_back(c);
            i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        out.push_back(c);
        i++;
    }
    return out;
}

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }
    bool eof() const { return i >= s.size(); }
    bool peek(char c) const { return !eof() && s[i] == c; }

    Json parse() {
        ws();
        if (eof()) return {};
        char c = s[i];
        if (c == '{') return parseObj();
        if (c == '[') return parseArr();
        if (c == '"') return Json{Json::Type::Str, false, 0, parseStr()};
        if (c == 't') { expect("true"); return Json{Json::Type::Bool, true, 0, ""}; }
        if (c == 'f') { expect("false"); return Json{Json::Type::Bool, false, 0, ""}; }
        if (c == 'n') { expect("null"); return {}; }
        return parseNum();
    }

    void expect(const char* word) {
        size_t l = strlen(word);
        if (i + l <= s.size() && s.compare(i, l, word) == 0) {
            i += l;
        }
    }

    std::string parseStr() {
        // assumes s[i] == '"'
        i++;
        std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\') {
                if (i >= s.size()) break;
                char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        // \uXXXX (basic; surrogate pairs produce UTF-8 via simple UCS-4 path)
                        unsigned cp = 0;
                        for (int k = 0; k < 4 && i < s.size(); k++) {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        }
                        // Surrogate pair support
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                            i += 2;
                            unsigned lo = 0;
                            for (int k = 0; k < 4 && i < s.size(); k++) {
                                char h = s[i++];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        if (cp < 0x80) {
                            out.push_back((char)cp);
                        } else if (cp < 0x800) {
                            out.push_back((char)(0xC0 | (cp >> 6)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else if (cp < 0x10000) {
                            out.push_back((char)(0xE0 | (cp >> 12)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back((char)(0xF0 | (cp >> 18)));
                            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    Json parseNum() {
        size_t start = i;
        if (!eof() && (s[i] == '-' || s[i] == '+')) i++;
        while (!eof() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+')) i++;
        double v = strtod(s.c_str() + start, nullptr);
        return Json{Json::Type::Num, false, v, ""};
    }

    Json parseArr() {
        Json j;
        j.type = Json::Type::Arr;
        i++;  // [
        ws();
        if (peek(']')) { i++; return j; }
        for (;;) {
            j.arr.push_back(parse());
            ws();
            if (peek(',')) { i++; ws(); continue; }
            if (peek(']')) { i++; break; }
            break;  // tolerate malformed tail
        }
        return j;
    }

    Json parseObj() {
        Json j;
        j.type = Json::Type::Obj;
        i++;  // {
        ws();
        if (peek('}')) { i++; return j; }
        for (;;) {
            ws();
            std::string key;
            if (peek('"')) {
                key = parseStr();
            } else {
                break;
            }
            ws();
            if (peek(':')) i++;
            ws();
            j.obj.emplace_back(std::move(key), parse());
            ws();
            if (peek(',')) { i++; continue; }
            if (peek('}')) { i++; break; }
            break;
        }
        return j;
    }
};

}  // namespace

Json parseJsonc(const std::string& src) {
    std::string clean = stripJsonc(src);
    Parser p{clean};
    return p.parse();
}