// Just enough JSON for MCP: parse a message, read its fields, and quote text for the answers. Strings are UTF-8.

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json* at(const char* k) const {
        if (kind != Obj) return nullptr;
        for (const auto& [key, v] : o)
            if (key == k) return &v;
        return nullptr;
    }
};

struct JsonParser {
    const char* p;
    const char* e;
    void ws() {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool lit(const char* w) {
        const size_t n = strlen(w);
        if (static_cast<size_t>(e - p) < n || memcmp(p, w, n) != 0) return false;
        p += n;
        return true;
    }
    static void put(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            const char c = *p++;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= e) return false;
            switch (*p++) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            put(out, cp);
                            cp = lo;
                        }
                    }
                    put(out, cp);
                    break;
                }
                default: return false;
            }
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return false;
        ws();
        if (p >= e) return false;
        const char c = *p;
        if (c == '{') {
            ++p;
            v.kind = Json::Obj;
            ws();
            if (p < e && *p == '}') {
                ++p;
                return true;
            }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                ++p;
                Json x;
                if (!value(x, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(x));
                ws();
                if (p < e && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < e && *p == '}') {
                    ++p;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++p;
            v.kind = Json::Arr;
            ws();
            if (p < e && *p == ']') {
                ++p;
                return true;
            }
            for (;;) {
                Json x;
                if (!value(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (p < e && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < e && *p == ']') {
                    ++p;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v.kind = Json::Str;
            return str(v.s);
        }
        if (lit("true")) {
            v.kind = Json::Bool;
            v.b = true;
            return true;
        }
        if (lit("false")) {
            v.kind = Json::Bool;
            return true;
        }
        if (lit("null")) return true;
        char* end = nullptr;
        v.n = strtod(p, &end);  // the text always ends in '\0' (it comes from a std::string)
        if (end == p) return false;
        v.kind = Json::Num;
        p = end;
        return true;
    }
};

bool JsonParse(const std::string& s, Json& v) {
    JsonParser jp{s.c_str(), s.c_str() + s.size()};
    if (!jp.value(v, 0)) return false;
    jp.ws();
    return jp.p == jp.e;
}

std::string JsonQuote(const std::string& s) {
    std::string out = "\"";
    out.reserve(s.size() + 2);
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

void JsonWrite(const Json& v, std::string& out) {
    switch (v.kind) {
        case Json::Null: out += "null"; break;
        case Json::Bool: out += v.b ? "true" : "false"; break;
        case Json::Num: {
            char b[64];
            snprintf(b, sizeof(b), "%.17g", v.n);
            out += b;
            break;
        }
        case Json::Str: out += JsonQuote(v.s); break;
        case Json::Arr:
            out += '[';
            for (size_t i = 0; i < v.a.size(); ++i) {
                if (i) out += ',';
                JsonWrite(v.a[i], out);
            }
            out += ']';
            break;
        case Json::Obj:
            out += '{';
            for (size_t i = 0; i < v.o.size(); ++i) {
                if (i) out += ',';
                out += JsonQuote(v.o[i].first) + ':';
                JsonWrite(v.o[i].second, out);
            }
            out += '}';
            break;
    }
}
