/*
 * Minimal JSON reader for Aulos bank files. Header only, no dependencies.
 *
 * Deliberate extensions over strict JSON, because humans edit these files:
 *   - // line comments and C style block comments
 *   - trailing commas in arrays and objects
 *
 * License: MIT.
 */
#ifndef AUL_JSON_H
#define AUL_JSON_H

#include <string>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cstdio>

namespace auljson {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool isNull()   const { return type == Type::Null; }
    bool isArray()  const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    const Value *find(const char *key) const {
        if (type != Type::Object) return nullptr;
        for (const auto &kv : object)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double numberOr(double fallback) const {
        if (type == Type::Number) return number;
        if (type == Type::Bool)   return boolean ? 1.0 : 0.0;
        return fallback;
    }
    bool boolOr(bool fallback) const {
        if (type == Type::Bool)   return boolean;
        if (type == Type::Number) return number != 0.0;
        return fallback;
    }
    std::string stringOr(const char *fallback) const {
        if (type == Type::String) return string;
        return std::string(fallback);
    }
    double memberNumber(const char *key, double fallback) const {
        const Value *v = find(key);
        return v ? v->numberOr(fallback) : fallback;
    }
    bool memberBool(const char *key, bool fallback) const {
        const Value *v = find(key);
        return v ? v->boolOr(fallback) : fallback;
    }
    std::string memberString(const char *key, const char *fallback) const {
        const Value *v = find(key);
        return v ? v->stringOr(fallback) : std::string(fallback);
    }
};

class Parser {
public:
    Parser(const char *text, size_t length) : p_(text), end_(text + length) {}

    bool parse(Value &out, std::string &error) {
        skip();
        if (!parseValue(out)) { error = error_; return false; }
        skip();
        if (p_ != end_) { fail("trailing characters after top level value"); error = error_; return false; }
        return true;
    }

private:
    const char *p_;
    const char *end_;
    std::string error_;

    bool fail(const char *msg) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s (at byte offset %ld)", msg, (long)(p_ - (end_ - (end_ - p_)) ));
        /* offset relative to start is not tracked separately, keep it simple */
        error_ = msg;
        (void)buf;
        return false;
    }

    void skip() {
        for (;;) {
            while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
            if (p_ + 1 < end_ && p_[0] == '/' && p_[1] == '/') {
                p_ += 2;
                while (p_ < end_ && *p_ != '\n') ++p_;
                continue;
            }
            if (p_ + 1 < end_ && p_[0] == '/' && p_[1] == '*') {
                p_ += 2;
                while (p_ + 1 < end_ && !(p_[0] == '*' && p_[1] == '/')) ++p_;
                if (p_ + 1 < end_) p_ += 2; else p_ = end_;
                continue;
            }
            return;
        }
    }

    bool literal(const char *word) {
        size_t n = 0;
        while (word[n]) ++n;
        if ((size_t)(end_ - p_) < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (p_[i] != word[i]) return false;
        p_ += n;
        return true;
    }

    static void appendUtf8(std::string &s, unsigned cp) {
        if (cp < 0x80) {
            s.push_back((char)cp);
        } else if (cp < 0x800) {
            s.push_back((char)(0xC0 | (cp >> 6)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back((char)(0xE0 | (cp >> 12)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            s.push_back((char)(0xF0 | (cp >> 18)));
            s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(unsigned &out) {
        if (end_ - p_ < 4) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = p_[i];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        p_ += 4;
        out = v;
        return true;
    }

    bool parseString(std::string &out) {
        if (p_ >= end_ || *p_ != '"') return fail("expected a string");
        ++p_;
        out.clear();
        while (p_ < end_) {
            char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= end_) return fail("unterminated escape sequence");
            char e = *p_++;
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 1 < end_ && p_[0] == '\\' && p_[1] == 'u') {
                        const char *save = p_;
                        p_ += 2;
                        unsigned lo = 0;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            p_ = save;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("unknown escape sequence");
            }
        }
        return fail("unterminated string");
    }

    bool parseValue(Value &out) {
        skip();
        if (p_ >= end_) return fail("unexpected end of input");
        char c = *p_;
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') {
            out.type = Type::String;
            return parseString(out.string);
        }
        if (literal("true"))  { out.type = Type::Bool; out.boolean = true;  return true; }
        if (literal("false")) { out.type = Type::Bool; out.boolean = false; return true; }
        if (literal("null"))  { out.type = Type::Null; return true; }
        /* number */
        char *stop = nullptr;
        double d = std::strtod(p_, &stop);
        if (stop == p_ || stop > end_) return fail("expected a value");
        p_ = stop;
        out.type = Type::Number;
        out.number = d;
        return true;
    }

    bool parseArray(Value &out) {
        out.type = Type::Array;
        ++p_; /* [ */
        skip();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        for (;;) {
            skip();
            if (p_ < end_ && *p_ == ']') { ++p_; return true; } /* trailing comma */
            Value v;
            if (!parseValue(v)) return false;
            out.array.push_back(std::move(v));
            skip();
            if (p_ < end_ && *p_ == ',') { ++p_; continue; }
            if (p_ < end_ && *p_ == ']') { ++p_; return true; }
            return fail("expected ',' or ']' in array");
        }
    }

    bool parseObject(Value &out) {
        out.type = Type::Object;
        ++p_; /* { */
        skip();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        for (;;) {
            skip();
            if (p_ < end_ && *p_ == '}') { ++p_; return true; } /* trailing comma */
            std::string key;
            if (!parseString(key)) return false;
            skip();
            if (p_ >= end_ || *p_ != ':') return fail("expected ':' after object key");
            ++p_;
            Value v;
            if (!parseValue(v)) return false;
            out.object.emplace_back(std::move(key), std::move(v));
            skip();
            if (p_ < end_ && *p_ == ',') { ++p_; continue; }
            if (p_ < end_ && *p_ == '}') { ++p_; return true; }
            return fail("expected ',' or '}' in object");
        }
    }
};

inline bool parse(const std::string &text, Value &out, std::string &error) {
    Parser parser(text.c_str(), text.size());
    return parser.parse(out, error);
}

} /* namespace auljson */

#endif /* AUL_JSON_H */
