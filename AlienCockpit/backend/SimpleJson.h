#pragma once
#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace SimpleJson {

    enum class Type { Null, Bool, Number, String, Array, Object };

    struct Value {
        Type type = Type::Null;
        bool boolVal = false;
        double numVal = 0.0;
        std::string strVal;
        std::vector<Value> arrVal;
        std::map<std::string, Value> objVal;

        Value() = default;
        Value(std::nullptr_t) : type(Type::Null) {}
        Value(bool b) : type(Type::Bool), boolVal(b) {}
        Value(int n) : type(Type::Number), numVal(static_cast<double>(n)) {}
        Value(unsigned int n) : type(Type::Number), numVal(static_cast<double>(n)) {}
        Value(long n) : type(Type::Number), numVal(static_cast<double>(n)) {}
        Value(unsigned long n) : type(Type::Number), numVal(static_cast<double>(n)) {}
        Value(double n) : type(Type::Number), numVal(n) {}
        Value(const char* s) : type(Type::String), strVal(s ? s : "") {}
        Value(const std::string& s) : type(Type::String), strVal(s) {}

        static Value Array() { Value v; v.type = Type::Array; return v; }
        static Value Object() { Value v; v.type = Type::Object; return v; }

        void push(const Value& v) {
            if (type != Type::Array) type = Type::Array;
            arrVal.push_back(v);
        }

        Value& operator[](const std::string& key) {
            if (type != Type::Object) type = Type::Object;
            return objVal[key];
        }

        bool has(const std::string& key) const {
            return type == Type::Object && objVal.find(key) != objVal.end();
        }

        const Value& get(const std::string& key) const {
            static const Value nullVal;
            if (type != Type::Object) return nullVal;
            auto it = objVal.find(key);
            return (it != objVal.end()) ? it->second : nullVal;
        }

        std::string asString(const std::string& def = "") const {
            return type == Type::String ? strVal : def;
        }
        int asInt(int def = 0) const {
            return type == Type::Number ? static_cast<int>(numVal) : def;
        }
        double asDouble(double def = 0.0) const {
            return type == Type::Number ? numVal : def;
        }
        bool asBool(bool def = false) const {
            return type == Type::Bool ? boolVal : def;
        }

        static std::string escapeString(const std::string& s) {
            std::string out = "\"";
            for (unsigned char c : s) {
                switch (c) {
                case '\"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
                }
            }
            out += "\"";
            return out;
        }

        std::string dump() const {
            switch (type) {
            case Type::Null: return "null";
            case Type::Bool: return boolVal ? "true" : "false";
            case Type::Number: {
                char buf[64];
                if (numVal == static_cast<long long>(numVal))
                    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(numVal));
                else
                    snprintf(buf, sizeof(buf), "%.4f", numVal);
                return buf;
            }
            case Type::String: return escapeString(strVal);
            case Type::Array: {
                std::string out = "[";
                for (size_t i = 0; i < arrVal.size(); ++i) {
                    if (i > 0) out += ",";
                    out += arrVal[i].dump();
                }
                out += "]";
                return out;
            }
            case Type::Object: {
                std::string out = "{";
                bool first = true;
                for (const auto& kv : objVal) {
                    if (!first) out += ",";
                    first = false;
                    out += escapeString(kv.first) + ":" + kv.second.dump();
                }
                out += "}";
                return out;
            }
            }
            return "null";
        }
    };

    class Parser {
        const std::string& s;
        size_t pos = 0;

        void skipWs() {
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        }

        std::string parseString() {
            std::string out;
            if (pos >= s.size() || s[pos] != '\"') return out;
            ++pos;
            while (pos < s.size()) {
                char c = s[pos++];
                if (c == '\"') break;
                if (c == '\\' && pos < s.size()) {
                    char esc = s[pos++];
                    switch (esc) {
                    case '\"': out += '\"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    default: out += esc; break;
                    }
                } else {
                    out += c;
                }
            }
            return out;
        }

    public:
        explicit Parser(const std::string& str) : s(str) {}

        Value parse() {
            skipWs();
            if (pos >= s.size()) return Value();
            char c = s[pos];
            if (c == '{') {
                Value obj = Value::Object();
                ++pos;
                skipWs();
                if (pos < s.size() && s[pos] == '}') { ++pos; return obj; }
                while (pos < s.size()) {
                    skipWs();
                    std::string key = parseString();
                    skipWs();
                    if (pos < s.size() && s[pos] == ':') ++pos;
                    obj[key] = parse();
                    skipWs();
                    if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                    if (pos < s.size() && s[pos] == '}') { ++pos; break; }
                    break;
                }
                return obj;
            }
            if (c == '[') {
                Value arr = Value::Array();
                ++pos;
                skipWs();
                if (pos < s.size() && s[pos] == ']') { ++pos; return arr; }
                while (pos < s.size()) {
                    arr.push(parse());
                    skipWs();
                    if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                    if (pos < s.size() && s[pos] == ']') { ++pos; break; }
                    break;
                }
                return arr;
            }
            if (c == '\"') {
                return Value(parseString());
            }
            if (c == 't' && s.compare(pos, 4, "true") == 0) { pos += 4; return Value(true); }
            if (c == 'f' && s.compare(pos, 5, "false") == 0) { pos += 5; return Value(false); }
            if (c == 'n' && s.compare(pos, 4, "null") == 0) { pos += 4; return Value(nullptr); }
            if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
                size_t start = pos;
                if (s[pos] == '-') ++pos;
                while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' || s[pos] == '+' || s[pos] == '-'))
                    ++pos;
                return Value(std::strtod(s.substr(start, pos - start).c_str(), nullptr));
            }
            ++pos;
            return Value();
        }
    };

    inline Value parse(const std::string& jsonStr) {
        Parser p(jsonStr);
        return p.parse();
    }
}
