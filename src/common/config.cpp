#include "common/config.h"

#include <openssl/rand.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <sys/stat.h>

namespace vmess {
namespace common {

namespace {

// ═══════════════════════════════════════════════════════════════════════════
// 极简 JSON 解析器：仅覆盖本项目配置文件所需子集
// （object / array / string / number / bool / null）
// ═══════════════════════════════════════════════════════════════════════════

class Json {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    static std::unique_ptr<Json> parse(const std::string& text);

    Kind kind() const { return kind_; }
    bool isObject() const { return kind_ == Kind::Object; }
    bool isArray() const { return kind_ == Kind::Array; }
    bool isString() const { return kind_ == Kind::String; }
    bool isBool() const { return kind_ == Kind::Bool; }
    bool isNumber() const { return kind_ == Kind::Number; }

    const std::string& asString() const { return str_; }
    bool asBool() const { return bool_; }
    long long asInt() const { return static_cast<long long>(num_); }

    size_t size() const { return arr_.size(); }
    const Json* at(size_t i) const {
        return (i < arr_.size()) ? arr_[i].get() : nullptr;
    }

    const Json* get(const std::string& key) const {
        for (const auto& kv : obj_) {
            if (kv.first == key) {
                return kv.second.get();
            }
        }
        return nullptr;
    }

private:
    friend class JsonParser;

    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<std::unique_ptr<Json>> arr_;
    std::vector<std::pair<std::string, std::unique_ptr<Json>>> obj_;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    std::unique_ptr<Json> parseDocument() {
        skipWs();
        auto v = parseValue();
        if (!v) {
            return nullptr;
        }
        skipWs();
        if (pos_ != s_.size()) {
            return nullptr;
        }
        return v;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skipWs() {
        while (pos_ < s_.size() &&
               std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::unique_ptr<Json> parseValue() {
        if (pos_ >= s_.size()) {
            return nullptr;
        }
        char c = s_[pos_];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't') return parseLiteral("true", Json::Kind::Bool, true);
        if (c == 'f') return parseLiteral("false", Json::Kind::Bool, false);
        if (c == 'n') return parseLiteral("null", Json::Kind::Null, false);
        return parseNumber();
    }

    std::unique_ptr<Json> parseLiteral(const char* lit, Json::Kind kind, bool val) {
        size_t len = std::strlen(lit);
        if (s_.compare(pos_, len, lit) != 0) {
            return nullptr;
        }
        pos_ += len;
        auto j = std::make_unique<Json>();
        j->kind_ = kind;
        j->bool_ = val;
        return j;
    }

    std::unique_ptr<Json> parseNumber() {
        size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' ||
                s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) {
            ++pos_;
        }
        if (pos_ == start) {
            return nullptr;
        }
        auto j = std::make_unique<Json>();
        j->kind_ = Json::Kind::Number;
        j->num_ = std::strtod(s_.c_str() + start, nullptr);
        return j;
    }

    std::unique_ptr<Json> parseString() {
        if (!consume('"')) {
            return nullptr;
        }
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') {
                auto j = std::make_unique<Json>();
                j->kind_ = Json::Kind::String;
                j->str_ = std::move(out);
                return j;
            }
            if (c == '\\') {
                if (pos_ >= s_.size()) return nullptr;
                char esc = s_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return nullptr;
                        unsigned code = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s_[pos_++];
                            int v = hexVal(h);
                            if (v < 0) return nullptr;
                            code = (code << 4) | static_cast<unsigned>(v);
                        }
                        // 仅处理 BMP 字符（配置中足够）
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return nullptr;
                }
            } else {
                out.push_back(c);
            }
        }
        return nullptr;
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    }

    std::unique_ptr<Json> parseArray() {
        if (!consume('[')) return nullptr;
        auto j = std::make_unique<Json>();
        j->kind_ = Json::Kind::Array;
        skipWs();
        if (consume(']')) return j;
        for (;;) {
            skipWs();
            auto v = parseValue();
            if (!v) return nullptr;
            j->arr_.push_back(std::move(v));
            skipWs();
            if (consume(']')) return j;
            if (!consume(',')) return nullptr;
        }
    }

    std::unique_ptr<Json> parseObject() {
        if (!consume('{')) return nullptr;
        auto j = std::make_unique<Json>();
        j->kind_ = Json::Kind::Object;
        skipWs();
        if (consume('}')) return j;
        for (;;) {
            skipWs();
            auto key = parseString();
            if (!key) return nullptr;
            skipWs();
            if (!consume(':')) return nullptr;
            skipWs();
            auto v = parseValue();
            if (!v) return nullptr;
            j->obj_.emplace_back(key->asString(), std::move(v));
            skipWs();
            if (consume('}')) return j;
            if (!consume(',')) return nullptr;
        }
    }
};

std::unique_ptr<Json> Json::parse(const std::string& text) {
    JsonParser p(text);
    return p.parseDocument();
}

// ── 工具 ──────────────────────────────────────────────────────────────────

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

bool ensureParentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return true;
    }
    std::string dir = path.substr(0, slash);
    if (dir.empty()) {
        return true;
    }
    // 逐级 mkdir（mkdir 对已存在目录不报错）
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        if (dir[i] == '/') {
            if (i > 0) {
                ::mkdir(cur.c_str(), 0755);
            }
            cur = dir.substr(0, i + 1);
        }
    }
    if (!cur.empty() && cur.back() != '/') {
        ::mkdir(dir.c_str(), 0755);
    } else if (cur.size() > 1 && cur.back() == '/') {
        ::mkdir(cur.substr(0, cur.size() - 1).c_str(), 0755);
    }
    return true;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 公开接口
// ═══════════════════════════════════════════════════════════════════════════

std::string generateUuid() {
    uint8_t bytes[16];
    bool ok = false;
    if (RAND_bytes(bytes, sizeof(bytes)) == 1) {
        ok = true;
    } else {
        // 保底：时间 + 进程内计数器（仅当 OpenSSL RAND 不可用时）
        static unsigned long counter = 0;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long seed = static_cast<unsigned long>(ts.tv_nsec) ^
                             reinterpret_cast<unsigned long>(&counter) ^ (counter++);
        for (int i = 0; i < 16; ++i) {
            bytes[i] = static_cast<uint8_t>((seed >> ((i % 8) * 8)) & 0xFF);
        }
    }
    (void)ok;
    // UUID v4：version=4, variant=10xx
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

bool writeServerConfig(const std::string& path, const ServerConfig& cfg,
                       std::string& error) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"port\": " << static_cast<int>(cfg.port) << ",\n";
    oss << "  \"host\": \"" << jsonEscape(cfg.host) << "\",\n";
    oss << "  \"log_level\": \"" << jsonEscape(cfg.logLevel) << "\",\n";
    oss << "  \"workers\": " << cfg.workers << ",\n";
    oss << "  \"tls\": {\n";
    oss << "    \"enabled\": " << (cfg.tls.enabled ? "true" : "false") << ",\n";
    oss << "    \"port\": " << static_cast<int>(cfg.tls.port) << ",\n";
    oss << "    \"cert_file\": \"" << jsonEscape(cfg.tls.certFile) << "\",\n";
    oss << "    \"key_file\": \"" << jsonEscape(cfg.tls.keyFile) << "\",\n";
    oss << "    \"cert_dir\": \"" << jsonEscape(cfg.tls.certDir) << "\",\n";
    oss << "    \"cert_days\": " << cfg.tls.certDays << "\n";
    oss << "  },\n";
    oss << "  \"users\": [\n";
    for (size_t i = 0; i < cfg.users.size(); ++i) {
        oss << "    {\"uuid\": \"" << jsonEscape(cfg.users[i].uuid)
            << "\", \"name\": \"" << jsonEscape(cfg.users[i].name) << "\"}";
        if (i + 1 < cfg.users.size()) {
            oss << ",";
        }
        oss << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";

    if (!ensureParentDir(path)) {
        error = "cannot create parent directory of " + path;
        return false;
    }
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        error = "cannot open " + path + " for writing";
        return false;
    }
    out << oss.str();
    out.close();
    if (!out) {
        error = "failed to write " + path;
        return false;
    }
    return true;
}

bool loadServerConfig(const std::string& path, ServerConfig& cfg,
                      bool* created, std::string& error) {
    if (created) {
        *created = false;
    }

    std::ifstream in(path);
    if (!in) {
        // ── 首次启动：生成默认配置（随机 UUID）并落盘 ──
        UserConfig uc;
        uc.uuid = generateUuid();
        uc.name = "default";
        cfg.users.push_back(std::move(uc));

        std::string writeErr;
        if (!writeServerConfig(path, cfg, writeErr)) {
            error = "config file not found, and failed to create one: " + writeErr;
            return false;
        }
        if (created) {
            *created = true;
        }
        return true;
    }

    std::stringstream ss;
    ss << in.rdbuf();
    auto root = Json::parse(ss.str());
    if (!root || !root->isObject()) {
        error = "invalid JSON in " + path;
        return false;
    }

    if (const Json* v = root->get("port"); v && v->isNumber()) {
        cfg.port = static_cast<uint16_t>(v->asInt());
    }
    if (const Json* v = root->get("host"); v && v->isString()) {
        cfg.host = v->asString();
    }
    if (const Json* v = root->get("log_level"); v && v->isString()) {
        cfg.logLevel = v->asString();
    }
    if (const Json* v = root->get("workers"); v && v->isNumber()) {
        cfg.workers = static_cast<int>(v->asInt());
    }

    if (const Json* t = root->get("tls"); t && t->isObject()) {
        if (const Json* v = t->get("enabled"); v && v->isBool()) {
            cfg.tls.enabled = v->asBool();
        }
        if (const Json* v = t->get("port"); v && v->isNumber()) {
            cfg.tls.port = static_cast<uint16_t>(v->asInt());
        }
        if (const Json* v = t->get("cert_file"); v && v->isString()) {
            cfg.tls.certFile = v->asString();
        }
        if (const Json* v = t->get("key_file"); v && v->isString()) {
            cfg.tls.keyFile = v->asString();
        }
        if (const Json* v = t->get("cert_dir"); v && v->isString()) {
            cfg.tls.certDir = v->asString();
        }
        if (const Json* v = t->get("cert_days"); v && v->isNumber()) {
            cfg.tls.certDays = static_cast<int>(v->asInt());
        }
    }

    if (const Json* u = root->get("users"); u && u->isArray()) {
        for (size_t i = 0; i < u->size(); ++i) {
            const Json* item = u->at(i);
            if (!item || !item->isObject()) {
                continue;
            }
            UserConfig uc;
            if (const Json* v = item->get("uuid"); v && v->isString()) {
                uc.uuid = v->asString();
            }
            if (const Json* v = item->get("name"); v && v->isString()) {
                uc.name = v->asString();
            }
            if (!uc.uuid.empty()) {
                cfg.users.push_back(std::move(uc));
            }
        }
    }
    return true;
}

bool loadClientConfig(const std::string& path, ClientConfig& cfg,
                      std::string& error) {
    error.clear();

    std::ifstream in(path);
    if (!in) {
        error = "config file not found: " + path + " (using defaults)";
        return true;
    }

    std::stringstream ss;
    ss << in.rdbuf();
    auto root = Json::parse(ss.str());
    if (!root || !root->isObject()) {
        error = "invalid JSON in " + path + " (using defaults)";
        return true;
    }

    if (const Json* v = root->get("socks5_port"); v && v->isNumber()) {
        cfg.socks5Port = static_cast<uint16_t>(v->asInt());
    }
    if (const Json* v = root->get("remote"); v && v->isString()) {
        cfg.remote = v->asString();
    }
    if (const Json* v = root->get("uuid"); v && v->isString()) {
        cfg.uuid = v->asString();
    }
    if (const Json* v = root->get("log_level"); v && v->isString()) {
        cfg.logLevel = v->asString();
    }
    if (const Json* v = root->get("workers"); v && v->isNumber()) {
        cfg.workers = static_cast<int>(v->asInt());
    }
    return true;
}

} // namespace common
} // namespace vmess
