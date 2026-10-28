#pragma once

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace json_minimal {

enum class JsonType { Null, Bool, Number, String, Array, Object };

struct JsonValue {
    JsonType                         type    = JsonType::Null;
    bool                             boolean = false;
    double                           number  = 0.0;
    std::string                      string;
    std::vector<JsonValue>           array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string text) : text_(std::move(text)) {}

    bool parse(JsonValue& out, std::string& error) {
        skip_ws();
        if (!parse_value(out, error)) return false;
        skip_ws();
        if (pos_ != text_.size()) {
            error = "trailing data after JSON value";
            return false;
        }
        return true;
    }

private:
    void skip_ws() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool parse_value(JsonValue& out, std::string& error) {
        skip_ws();
        if (pos_ >= text_.size()) {
            error = "unexpected end of JSON";
            return false;
        }
        const char c = text_[pos_];
        if (c == '{') return parse_object(out, error);
        if (c == '[') return parse_array(out, error);
        if (c == '"') return parse_string(out, error);
        if (c == 't' || c == 'f') return parse_bool(out, error);
        if (c == 'n') return parse_null(out, error);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parse_number(out, error);
        error = "unexpected character '";
        error.push_back(c);
        error.push_back('\'');
        return false;
    }

    bool parse_object(JsonValue& out, std::string& error) {
        out      = JsonValue{};
        out.type = JsonType::Object;
        ++pos_;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                error = "expected string object key";
                return false;
            }
            JsonValue key;
            if (!parse_string(key, error)) return false;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                error = "expected ':' after object key";
                return false;
            }
            ++pos_;
            JsonValue value;
            if (!parse_value(value, error)) return false;
            out.object.emplace(std::move(key.string), std::move(value));
            skip_ws();
            if (pos_ >= text_.size()) {
                error = "expected ',' or '}' in object";
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            error = "expected ',' or '}' in object";
            return false;
        }
    }

    bool parse_array(JsonValue& out, std::string& error) {
        out      = JsonValue{};
        out.type = JsonType::Array;
        ++pos_;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue element;
            if (!parse_value(element, error)) return false;
            out.array.push_back(std::move(element));
            skip_ws();
            if (pos_ >= text_.size()) {
                error = "expected ',' or ']' in array";
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            error = "expected ',' or ']' in array";
            return false;
        }
    }

    bool parse_string(JsonValue& out, std::string& error) {
        out      = JsonValue{};
        out.type = JsonType::String;
        ++pos_;
        std::string value;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            const unsigned char raw = static_cast<unsigned char>(text_[pos_]);
            if (raw < 0x20) {
                error = "unescaped control character in string";
                return false;
            }
            if (text_[pos_] != '\\') {
                value.push_back(text_[pos_++]);
                continue;
            }
            if (++pos_ >= text_.size()) {
                error = "unexpected end of string escape";
                return false;
            }
            const char escaped = text_[pos_++];
            switch (escaped) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case '/':
                value.push_back('/');
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u': {
                if (pos_ + 4 > text_.size()) {
                    error = "incomplete unicode escape";
                    return false;
                }
                for (size_t i = 0; i < 4; ++i) {
                    if (!std::isxdigit(static_cast<unsigned char>(text_[pos_ + i]))) {
                        error = "invalid unicode escape";
                        return false;
                    }
                }
                value.append("\\u");
                value.append(text_, pos_, 4);
                pos_ += 4;
                break;
            }
            default:
                error = "invalid string escape";
                return false;
            }
        }
        if (pos_ >= text_.size()) {
            error = "unterminated string";
            return false;
        }
        ++pos_;
        out.string = std::move(value);
        return true;
    }

    bool parse_number(JsonValue& out, std::string& error) {
        const size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) {
            error = "invalid JSON number";
            return false;
        }
        if (text_[pos_] == '0') {
            ++pos_;
            if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                error = "leading zero in JSON number";
                return false;
            }
        } else if (std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        } else {
            error = "invalid JSON number";
            return false;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            const size_t fraction_start = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
            if (pos_ == fraction_start) {
                error = "missing fraction digits";
                return false;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            const size_t exponent_start = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
            if (pos_ == exponent_start) {
                error = "missing exponent digits";
                return false;
            }
        }

        const std::string token = text_.substr(start, pos_ - start);
        char*             end   = nullptr;
        errno                   = 0;
        const double number     = std::strtod(token.c_str(), &end);
        if (errno == ERANGE || end != token.c_str() + token.size() || !std::isfinite(number)) {
            error = "JSON number out of range";
            return false;
        }
        out        = JsonValue{};
        out.type   = JsonType::Number;
        out.number = number;
        return true;
    }

    bool parse_bool(JsonValue& out, std::string& error) {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out         = JsonValue{};
            out.type    = JsonType::Bool;
            out.boolean = true;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out         = JsonValue{};
            out.type    = JsonType::Bool;
            out.boolean = false;
            return true;
        }
        error = "expected boolean";
        return false;
    }

    bool parse_null(JsonValue& out, std::string& error) {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out      = JsonValue{};
            out.type = JsonType::Null;
            return true;
        }
        error = "expected null";
        return false;
    }

    std::string text_;
    size_t      pos_ = 0;
};

inline void serialize_string(std::ostream& os, const std::string& value) {
    os << '"';
    for (const char c : value) {
        switch (c) {
        case '"':
            os << "\\\"";
            break;
        case '\\':
            os << "\\\\";
            break;
        case '\b':
            os << "\\b";
            break;
        case '\f':
            os << "\\f";
            break;
        case '\n':
            os << "\\n";
            break;
        case '\r':
            os << "\\r";
            break;
        case '\t':
            os << "\\t";
            break;
        default:
            os << c;
            break;
        }
    }
    os << '"';
}

inline void serialize_indent(std::ostream& os, int indent) {
    for (int i = 0; i < indent; ++i)
        os << "  ";
}

inline void serialize_value(std::ostream& os, const JsonValue& value, int indent) {
    switch (value.type) {
    case JsonType::Null:
        os << "null";
        break;
    case JsonType::Bool:
        os << (value.boolean ? "true" : "false");
        break;
    case JsonType::Number: {
        if (std::floor(value.number) == value.number && value.number >= -9e15 &&
            value.number <= 9e15) {
            os << static_cast<int64_t>(value.number);
        } else {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.9g", value.number);
            os << buffer;
        }
        break;
    }
    case JsonType::String:
        serialize_string(os, value.string);
        break;
    case JsonType::Array: {
        os << "[\n";
        for (size_t i = 0; i < value.array.size(); ++i) {
            serialize_indent(os, indent + 1);
            serialize_value(os, value.array[i], indent + 1);
            if (i + 1 < value.array.size()) os << ',';
            os << '\n';
        }
        serialize_indent(os, indent);
        os << ']';
        break;
    }
    case JsonType::Object: {
        os << "{\n";
        size_t i = 0;
        for (const auto& item : value.object) {
            serialize_indent(os, indent + 1);
            serialize_string(os, item.first);
            os << ": ";
            serialize_value(os, item.second, indent + 1);
            if (++i < value.object.size()) os << ',';
            os << '\n';
        }
        serialize_indent(os, indent);
        os << '}';
        break;
    }
    }
}

inline const JsonValue* find(const JsonValue& value, const char* key) {
    if (value.type != JsonType::Object) return nullptr;
    const auto it = value.object.find(key);
    return it != value.object.end() ? &it->second : nullptr;
}

inline bool as_int(const JsonValue& value, int& out) {
    if (value.type != JsonType::Number || !std::isfinite(value.number) ||
        std::floor(value.number) != value.number ||
        value.number < static_cast<double>(std::numeric_limits<int>::min()) ||
        value.number > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    out = static_cast<int>(value.number);
    return true;
}

inline bool as_int64(const JsonValue& value, int64_t& out) {
    constexpr double MAX_EXACT_INTEGER = 9007199254740991.0;
    if (value.type != JsonType::Number || !std::isfinite(value.number) ||
        std::floor(value.number) != value.number || value.number < -MAX_EXACT_INTEGER ||
        value.number > MAX_EXACT_INTEGER) {
        return false;
    }
    out = static_cast<int64_t>(value.number);
    return true;
}

inline bool as_float(const JsonValue& value, float& out) {
    if (value.type != JsonType::Number || !std::isfinite(value.number) ||
        value.number < -std::numeric_limits<float>::max() ||
        value.number > std::numeric_limits<float>::max()) {
        return false;
    }
    out = static_cast<float>(value.number);
    return std::isfinite(out);
}

inline bool as_string(const JsonValue& value, std::string& out) {
    if (value.type != JsonType::String) return false;
    out = value.string;
    return true;
}

} // namespace json_minimal
