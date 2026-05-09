#include "ecs/debug_server.hpp"

#if ECS_ENABLE_DEBUG_SERVER

#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ecs {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) {
    closesocket(socket);
}
bool would_block() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
}
bool set_nonblocking(Socket socket) {
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) {
    close(socket);
}
bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
bool set_nonblocking(Socket socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::string quoted(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

std::string entity_id(Entity entity) {
    return std::to_string(entity.value);
}

Entity parse_entity_id(const std::string& value) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return Entity{};
    }
    return Entity{static_cast<std::uint64_t>(parsed)};
}

const char* kind_name(EntityKind kind) {
    switch (kind) {
    case EntityKind::Invalid: return "Invalid";
    case EntityKind::User: return "User";
    case EntityKind::Component: return "Component";
    case EntityKind::Job: return "Job";
    case EntityKind::System: return "System";
    }
    return "Invalid";
}

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::map<std::string, JsonValue> object;

    const JsonValue* get(const std::string& key) const {
        const auto found = object.find(key);
        return found != object.end() ? &found->second : nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    JsonValue parse() {
        skip_ws();
        return parse_value();
    }

private:
    JsonValue parse_value() {
        skip_ws();
        if (at_end()) {
            return {};
        }
        if (peek() == '"') {
            JsonValue value;
            value.kind = JsonValue::Kind::String;
            value.string = parse_string();
            return value;
        }
        if (peek() == '{') {
            return parse_object();
        }
        if (match("true")) {
            JsonValue value;
            value.kind = JsonValue::Kind::Bool;
            value.boolean = true;
            return value;
        }
        if (match("false")) {
            JsonValue value;
            value.kind = JsonValue::Kind::Bool;
            return value;
        }
        if (match("null")) {
            return {};
        }
        return parse_number();
    }

    JsonValue parse_object() {
        JsonValue value;
        value.kind = JsonValue::Kind::Object;
        ++pos_;
        skip_ws();
        while (!at_end() && peek() != '}') {
            if (peek() != '"') {
                break;
            }
            std::string key = parse_string();
            skip_ws();
            if (!at_end() && peek() == ':') {
                ++pos_;
            }
            value.object.emplace(std::move(key), parse_value());
            skip_ws();
            if (!at_end() && peek() == ',') {
                ++pos_;
                skip_ws();
            }
        }
        if (!at_end() && peek() == '}') {
            ++pos_;
        }
        return value;
    }

    JsonValue parse_number() {
        const std::size_t begin = pos_;
        if (!at_end() && peek() == '-') {
            ++pos_;
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
        if (!at_end() && peek() == '.') {
            ++pos_;
            while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        value.number = std::strtod(text_.substr(begin, pos_ - begin).c_str(), nullptr);
        return value;
    }

    std::string parse_string() {
        std::string out;
        ++pos_;
        while (!at_end() && peek() != '"') {
            char ch = text_[pos_++];
            if (ch == '\\' && !at_end()) {
                ch = text_[pos_++];
                switch (ch) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(ch); break;
                }
            } else {
                out.push_back(ch);
            }
        }
        if (!at_end() && peek() == '"') {
            ++pos_;
        }
        return out;
    }

    bool match(const char* literal) {
        const std::size_t length = std::strlen(literal);
        if (text_.compare(pos_, length, literal) != 0) {
            return false;
        }
        pos_ += length;
        return true;
    }

    void skip_ws() {
        while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
    }

    bool at_end() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    const std::string& text_;
    std::size_t pos_ = 0;
};

std::string json_value_to_string(const JsonValue& value) {
    switch (value.kind) {
    case JsonValue::Kind::String: return value.string;
    case JsonValue::Kind::Number: {
        std::ostringstream out;
        out << static_cast<std::int64_t>(value.number);
        return out.str();
    }
    default: return {};
    }
}

std::string variable_string(const JsonValue& request, const std::string& name) {
    const JsonValue* variables = request.get("variables");
    if (variables == nullptr || variables->kind != JsonValue::Kind::Object) {
        return {};
    }
    const JsonValue* value = variables->get(name);
    return value != nullptr ? json_value_to_string(*value) : std::string{};
}

std::string inline_arg(const std::string& query, const std::string& name) {
    const std::string needle = name + ":";
    const std::size_t found = query.find(needle);
    if (found == std::string::npos) {
        return {};
    }
    std::size_t pos = found + needle.size();
    while (pos < query.size() && std::isspace(static_cast<unsigned char>(query[pos]))) {
        ++pos;
    }
    if (pos < query.size() && query[pos] == '"') {
        const std::size_t end = query.find('"', pos + 1);
        return end != std::string::npos ? query.substr(pos + 1, end - pos - 1) : std::string{};
    }
    const std::size_t end = query.find_first_of(",) \t\r\n", pos);
    return query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

std::string operation_arg(const JsonValue& request, const std::string& query, const std::string& name) {
    std::string value = variable_string(request, name);
    if (!value.empty()) {
        return value;
    }
    return inline_arg(query, name);
}

void append_entity(std::ostringstream& out, Registry& registry, Entity entity, const std::string& display_name = {}) {
    out << "{\"id\":" << quoted(entity_id(entity))
        << ",\"index\":" << Registry::entity_index(entity)
        << ",\"version\":" << Registry::entity_version(entity)
        << ",\"kind\":" << quoted(kind_name(registry.entity_kind(entity)));
    if (!display_name.empty()) {
        out << ",\"displayName\":" << quoted(display_name);
    }
    out << "}";
}

void append_component_ref(std::ostringstream& out, Registry& registry, Entity component) {
    bool singleton = false;
    const std::vector<EntityComponentInfo> singletons = registry.singleton_components();
    singleton = std::any_of(singletons.begin(), singletons.end(), [&](const EntityComponentInfo& info) {
        return info.component == component;
    });
    out << "{\"component\":" << quoted(entity_id(component))
        << ",\"name\":" << quoted(registry.component_name(component))
        << ",\"singleton\":" << (singleton ? "true" : "false") << "}";
}

void append_registered_tag(std::ostringstream& out, Registry& registry, Entity tag) {
    out << "{\"component\":" << quoted(entity_id(tag))
        << ",\"name\":" << quoted(registry.component_name(tag)) << "}";
}

std::string error_response(const std::string& message) {
    return "{\"errors\":[{\"message\":" + quoted(message) + "}]}";
}

bool write_field_value(Registry& registry, Entity component, const ComponentField& field, const JsonValue& value, unsigned char* bytes) {
    if (field.type == registry.primitive_type(PrimitiveType::Bool)) {
        if (field.count != 1) {
            return false;
        }
        if (value.kind != JsonValue::Kind::Bool) {
            return false;
        }
        *reinterpret_cast<bool*>(bytes + field.offset) = value.boolean;
        return true;
    }
    if (field.type == registry.primitive_type(PrimitiveType::String)) {
        if (field.count == 0 || value.kind != JsonValue::Kind::String) {
            return false;
        }
        unsigned char* target = bytes + field.offset;
        std::memset(target, 0, field.count);
        const std::size_t limit = field.count > 0 ? field.count - 1 : 0;
        const std::size_t length = value.string.size() < limit ? value.string.size() : limit;
        for (std::size_t i = 0; i < length; ++i) {
            const unsigned char ch = static_cast<unsigned char>(value.string[i]);
            target[i] = ch >= 0x20 && ch <= 0x7e ? ch : static_cast<unsigned char>('?');
        }
        (void)component;
        return true;
    }
    if (field.count != 1) {
        return false;
    }
    if (value.kind != JsonValue::Kind::Number) {
        return false;
    }
    if (field.type == registry.primitive_type(PrimitiveType::I32)) {
        *reinterpret_cast<std::int32_t*>(bytes + field.offset) = static_cast<std::int32_t>(value.number);
    } else if (field.type == registry.primitive_type(PrimitiveType::U32)) {
        *reinterpret_cast<std::uint32_t*>(bytes + field.offset) = static_cast<std::uint32_t>(value.number);
    } else if (field.type == registry.primitive_type(PrimitiveType::I64)) {
        *reinterpret_cast<std::int64_t*>(bytes + field.offset) = static_cast<std::int64_t>(value.number);
    } else if (field.type == registry.primitive_type(PrimitiveType::U64)) {
        *reinterpret_cast<std::uint64_t*>(bytes + field.offset) = static_cast<std::uint64_t>(value.number);
    } else if (field.type == registry.primitive_type(PrimitiveType::F32)) {
        *reinterpret_cast<float*>(bytes + field.offset) = static_cast<float>(value.number);
    } else if (field.type == registry.primitive_type(PrimitiveType::F64)) {
        *reinterpret_cast<double*>(bytes + field.offset) = value.number;
    } else {
        (void)component;
        return false;
    }
    return true;
}

const char* editable_field_type(Registry& registry, const ComponentField& field) {
    if (field.type == registry.primitive_type(PrimitiveType::Bool)) {
        return field.count == 1 ? "bool" : nullptr;
    }
    if (field.type == registry.primitive_type(PrimitiveType::String)) {
        return field.count > 0 ? "string" : nullptr;
    }
    if (field.count != 1) {
        return nullptr;
    }
    if (field.type == registry.primitive_type(PrimitiveType::I32)) {
        return "i32";
    }
    if (field.type == registry.primitive_type(PrimitiveType::U32)) {
        return "u32";
    }
    if (field.type == registry.primitive_type(PrimitiveType::I64)) {
        return "i64";
    }
    if (field.type == registry.primitive_type(PrimitiveType::U64)) {
        return "u64";
    }
    if (field.type == registry.primitive_type(PrimitiveType::F32)) {
        return "f32";
    }
    if (field.type == registry.primitive_type(PrimitiveType::F64)) {
        return "f64";
    }
    return nullptr;
}

void append_field_value(std::ostringstream& out, Registry& registry, const ComponentField& field, const unsigned char* bytes) {
    if (field.type == registry.primitive_type(PrimitiveType::Bool)) {
        out << (*reinterpret_cast<const bool*>(bytes + field.offset) ? "true" : "false");
    } else if (field.type == registry.primitive_type(PrimitiveType::String)) {
        std::string value;
        value.reserve(field.count);
        const unsigned char* source = bytes + field.offset;
        for (std::size_t i = 0; i < field.count && source[i] != '\0'; ++i) {
            const unsigned char ch = source[i];
            value.push_back(ch >= 0x20 && ch <= 0x7e ? static_cast<char>(ch) : '?');
        }
        out << quoted(value);
    } else if (field.type == registry.primitive_type(PrimitiveType::I32)) {
        out << quoted(std::to_string(*reinterpret_cast<const std::int32_t*>(bytes + field.offset)));
    } else if (field.type == registry.primitive_type(PrimitiveType::U32)) {
        out << quoted(std::to_string(*reinterpret_cast<const std::uint32_t*>(bytes + field.offset)));
    } else if (field.type == registry.primitive_type(PrimitiveType::I64)) {
        out << quoted(std::to_string(*reinterpret_cast<const std::int64_t*>(bytes + field.offset)));
    } else if (field.type == registry.primitive_type(PrimitiveType::U64)) {
        out << quoted(std::to_string(*reinterpret_cast<const std::uint64_t*>(bytes + field.offset)));
    } else if (field.type == registry.primitive_type(PrimitiveType::F32)) {
        std::ostringstream value;
        value << *reinterpret_cast<const float*>(bytes + field.offset);
        out << quoted(value.str());
    } else if (field.type == registry.primitive_type(PrimitiveType::F64)) {
        std::ostringstream value;
        value << *reinterpret_cast<const double*>(bytes + field.offset);
        out << quoted(value.str());
    } else {
        out << "null";
    }
}

}  // namespace

class DebugServer::Impl {
public:
    Impl(Registry& registry, DebugServerOptions options)
        : registry_(registry), options_(std::move(options)) {
        if (!options_.enabled) {
            return;
        }
        debug_name_component_ = registry_.register_component<DebugName>("DebugName");
        registry_.set_component_fields(
            debug_name_component_,
            {ComponentField{
                "name",
                offsetof(DebugName, bytes),
                registry_.primitive_type(PrimitiveType::String),
                DebugName::storage_size}});
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        listen_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_socket_ == invalid_socket) {
            last_error_ = "failed to create socket";
            return;
        }

        int reuse = 1;
        setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options_.port);
        address.sin_addr.s_addr = inet_addr(options_.bind_address.c_str());
        if (address.sin_addr.s_addr == INADDR_NONE) {
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }

        if (::bind(listen_socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            last_error_ = "failed to bind debug server socket";
            close_socket(listen_socket_);
            listen_socket_ = invalid_socket;
            return;
        }
        if (::listen(listen_socket_, 16) != 0 || !set_nonblocking(listen_socket_)) {
            last_error_ = "failed to listen on debug server socket";
            close_socket(listen_socket_);
            listen_socket_ = invalid_socket;
            return;
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int length = sizeof(bound);
#else
        socklen_t length = sizeof(bound);
#endif
        if (getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
            bound_port_ = ntohs(bound.sin_port);
        }
        active_ = true;
    }

    ~Impl() {
        for (Client& client : clients_) {
            close_socket(client.socket);
        }
        if (listen_socket_ != invalid_socket) {
            close_socket(listen_socket_);
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void poll() {
        if (!active_) {
            return;
        }
        accept_clients();
        std::size_t handled = 0;
        for (std::size_t i = 0; i < clients_.size() && handled < options_.max_requests_per_poll;) {
            Client& client = clients_[i];
            read_client(client);
            if (request_complete(client)) {
                if (request_method(client.buffer) == "OPTIONS") {
                    write_options_response(client.socket);
                } else {
                    const std::string body = request_body(client.buffer);
                    const std::string response = handle_graphql(body);
                    write_response(client.socket, response);
                }
                close_socket(client.socket);
                clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
                ++handled;
                continue;
            }
            ++i;
        }
    }

    bool enabled() const noexcept { return active_; }
    std::uint16_t port() const noexcept { return bound_port_; }
    const std::string& last_error() const noexcept { return last_error_; }

private:
    struct Client {
        Socket socket = invalid_socket;
        std::string buffer;
    };

    void accept_clients() {
        while (true) {
            Socket client = ::accept(listen_socket_, nullptr, nullptr);
            if (client == invalid_socket) {
                if (!would_block()) {
                    last_error_ = "failed to accept debug client";
                }
                return;
            }
            set_nonblocking(client);
            clients_.push_back(Client{client, {}});
        }
    }

    void read_client(Client& client) {
        char chunk[4096];
        while (true) {
#ifdef _WIN32
            const int count = recv(client.socket, chunk, sizeof(chunk), 0);
#else
            const ssize_t count = recv(client.socket, chunk, sizeof(chunk), 0);
#endif
            if (count > 0) {
                client.buffer.append(chunk, static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0 || would_block()) {
                return;
            }
            last_error_ = "failed to read debug client";
            return;
        }
    }

    static std::size_t content_length(const std::string& request) {
        const std::string key = "Content-Length:";
        std::size_t found = request.find(key);
        if (found == std::string::npos) {
            found = request.find("content-length:");
        }
        if (found == std::string::npos) {
            return 0;
        }
        found += key.size();
        while (found < request.size() && std::isspace(static_cast<unsigned char>(request[found]))) {
            ++found;
        }
        return static_cast<std::size_t>(std::strtoull(request.c_str() + found, nullptr, 10));
    }

    static bool request_complete(const Client& client) {
        const std::size_t header_end = client.buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return false;
        }
        return client.buffer.size() >= header_end + 4 + content_length(client.buffer);
    }

    static std::string request_body(const std::string& request) {
        const std::size_t header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return {};
        }
        return request.substr(header_end + 4, content_length(request));
    }

    static std::string request_method(const std::string& request) {
        const std::size_t end = request.find(' ');
        return end == std::string::npos ? std::string{} : request.substr(0, end);
    }

    static void write_options_response(Socket socket) {
        std::ostringstream response;
        response << "HTTP/1.1 204 No Content\r\n"
                 << "Access-Control-Allow-Origin: *\r\n"
                 << "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
                 << "Access-Control-Allow-Headers: Content-Type\r\n"
                 << "Access-Control-Max-Age: 600\r\n"
                 << "Connection: close\r\n"
                 << "Content-Length: 0\r\n\r\n";
        const std::string bytes = response.str();
        const char* cursor = bytes.data();
        std::size_t remaining = bytes.size();
        while (remaining > 0) {
#ifdef _WIN32
            const int sent = send(socket, cursor, static_cast<int>(remaining), 0);
#else
            const ssize_t sent = send(socket, cursor, remaining, 0);
#endif
            if (sent <= 0) {
                return;
            }
            cursor += sent;
            remaining -= static_cast<std::size_t>(sent);
        }
    }

    static void write_response(Socket socket, const std::string& body) {
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Access-Control-Allow-Origin: *\r\n"
                 << "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
                 << "Access-Control-Allow-Headers: Content-Type\r\n"
                 << "Connection: close\r\n"
                 << "Content-Length: " << body.size() << "\r\n\r\n"
                 << body;
        const std::string bytes = response.str();
        const char* cursor = bytes.data();
        std::size_t remaining = bytes.size();
        while (remaining > 0) {
#ifdef _WIN32
            const int sent = send(socket, cursor, static_cast<int>(remaining), 0);
#else
            const ssize_t sent = send(socket, cursor, remaining, 0);
#endif
            if (sent <= 0) {
                return;
            }
            cursor += sent;
            remaining -= static_cast<std::size_t>(sent);
        }
    }

    std::string handle_graphql(const std::string& body) {
        JsonValue request = JsonParser(body).parse();
        const JsonValue* query_value = request.get("query");
        const std::string query =
            query_value != nullptr && query_value->kind == JsonValue::Kind::String ? query_value->string : body;
        try {
            if (query.find("__schema") != std::string::npos || query.find("__type") != std::string::npos) {
                return schema_response();
            }
            if (query.find("removeComponent") != std::string::npos) {
                return remove_component_response(request, query);
            }
            if (query.find("setComponent") != std::string::npos) {
                return set_component_response(request, query);
            }
            if (query.find("entity(") != std::string::npos) {
                return entity_response(parse_entity_id(operation_arg(request, query, "id")));
            }
            if (query.find("job(") != std::string::npos) {
                return job_response(parse_entity_id(operation_arg(request, query, "id")));
            }
            if (query.find("registeredTags") != std::string::npos) {
                return registered_tags_response();
            }
            if (query.find("serverName") != std::string::npos) {
                return server_name_response();
            }
            if (query.find("singletons") != std::string::npos) {
                return singletons_response();
            }
            if (query.find("jobs") != std::string::npos) {
                return jobs_response();
            }
            if (query.find("entities") != std::string::npos) {
                return entities_response();
            }
        } catch (const std::exception& exception) {
            return error_response(exception.what());
        }
        return error_response("unsupported GraphQL operation");
    }

    std::string schema_response() const {
        return "{\"data\":{\"__schema\":{\"queryType\":{\"name\":\"Query\"},\"mutationType\":{\"name\":\"Mutation\"},"
               "\"types\":[{\"name\":\"Query\"},{\"name\":\"Mutation\"},{\"name\":\"Entity\"},{\"name\":\"Job\"},"
               "{\"name\":\"EntityDetail\"},{\"name\":\"JobDetail\"},{\"name\":\"ComponentInstance\"},"
               "{\"name\":\"SingletonInstance\"},"
               "{\"name\":\"ComponentFieldValue\"},{\"name\":\"RegisteredTag\"}]}}}";
    }

    std::string server_name_response() const {
        return "{\"data\":{\"serverName\":" + quoted(options_.name) + "}}";
    }

    std::string entities_response() {
        const std::map<std::uint64_t, std::string> display_names = entity_display_names();
        std::ostringstream out;
        out << "{\"data\":{\"entities\":[";
        bool first = true;
        registry_.view<>().each([&](Entity entity) {
            if (entity == registry_.singleton_storage_entity()) {
                return;
            }
            if (!first) {
                out << ",";
            }
            first = false;
            const auto display_name = display_names.find(entity.value);
            append_entity(
                out,
                registry_,
                entity,
                display_name != display_names.end() ? display_name->second : std::string{});
        });
        out << "]}}";
        return out.str();
    }

    std::string singletons_response() {
        const Entity singleton_entity = registry_.singleton_storage_entity();
        std::ostringstream out;
        out << "{\"data\":{\"singletons\":[";
        if (singleton_entity) {
            const std::vector<EntityComponentInfo> components = registry_.singleton_components();
            for (std::size_t i = 0; i < components.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << "{\"entity\":";
                append_entity(out, registry_, singleton_entity);
                out << ",\"component\":";
                append_component(out, singleton_entity, components[i]);
                out << "}";
            }
        }
        out << "]}}";
        return out.str();
    }

    std::string jobs_response() {
        std::ostringstream out;
        out << "{\"data\":{\"jobs\":[";
        bool first = true;
        registry_.view<>().with_tags({registry_.job_tag()}).each([&](Entity job) {
            const std::optional<JobInfo> info = registry_.job_info(job);
            if (!info) {
                return;
            }
            if (!first) {
                out << ",";
            }
            first = false;
            append_job(out, *info);
        });
        out << "]}}";
        return out.str();
    }

    std::string registered_tags_response() {
        std::ostringstream out;
        out << "{\"data\":{\"registeredTags\":[";
        bool first = true;
        registry_.view<>().each([&](Entity entity) {
            const ComponentInfo* info = registry_.component_info(entity);
            if (info == nullptr || !info->tag || registry_.has(entity, registry_.system_tag())) {
                return;
            }
            if (!first) {
                out << ",";
            }
            first = false;
            append_registered_tag(out, registry_, entity);
        });
        out << "]}}";
        return out.str();
    }

    std::string entity_response(Entity entity) {
        if (!registry_.alive(entity)) {
            return "{\"data\":{\"entity\":null}}";
        }
        std::ostringstream out;
        out << "{\"data\":{\"entity\":";
        append_entity_detail(out, entity);
        out << "}}";
        return out.str();
    }

    std::string job_response(Entity job) {
        const std::optional<JobInfo> info = registry_.job_info(job);
        if (!info) {
            return "{\"data\":{\"job\":null}}";
        }
        std::ostringstream out;
        out << "{\"data\":{\"job\":";
        append_job_detail(out, *info);
        out << "}}";
        return out.str();
    }

    std::string remove_component_response(const JsonValue& request, const std::string& query) {
        const Entity entity = parse_entity_id(operation_arg(request, query, "entity"));
        const Entity component = parse_entity_id(operation_arg(request, query, "component"));
        bool removed = false;
        const ComponentInfo* info = registry_.component_info(component);
        if (info == nullptr) {
            return error_response("component is not registered");
        }
        if (info->tag) {
            removed = registry_.remove_tag(entity, component);
        } else {
            removed = registry_.remove(entity, component);
        }
        return std::string("{\"data\":{\"removeComponent\":{\"ok\":") + (removed ? "true" : "false") + "}}}";
    }

    std::string set_component_response(const JsonValue& request, const std::string& query) {
        const Entity entity = parse_entity_id(operation_arg(request, query, "entity"));
        const Entity component = parse_entity_id(operation_arg(request, query, "component"));
        const ComponentInfo* info = registry_.component_info(component);
        if (info == nullptr) {
            return error_response("component is not registered");
        }
        if (info->tag) {
            if (!registry_.add_tag(entity, component)) {
                return error_response("failed to add tag");
            }
        } else {
            if (!info->trivially_copyable) {
                return error_response("component is not writable through runtime field values");
            }
            std::vector<unsigned char> bytes(info->size, 0);
            if (const void* existing = registry_.get(entity, component)) {
                std::memcpy(bytes.data(), existing, info->size);
            }
            const JsonValue* variables = request.get("variables");
            const JsonValue* value = variables != nullptr ? variables->get("value") : nullptr;
            if (value == nullptr || value->kind != JsonValue::Kind::Object) {
                return error_response("setComponent value must be an object");
            }
            const std::vector<ComponentField>* fields = registry_.component_fields(component);
            if (fields == nullptr) {
                return error_response("component has no field metadata");
            }
            for (const auto& entry : value->object) {
                const auto field = std::find_if(fields->begin(), fields->end(), [&](const ComponentField& candidate) {
                    return candidate.name == entry.first;
                });
                if (field == fields->end() || !write_field_value(registry_, component, *field, entry.second, bytes.data())) {
                    return error_response("invalid component field value");
                }
            }
            if (registry_.add(entity, component, bytes.data()) == nullptr) {
                return error_response("failed to set component");
            }
        }
        std::ostringstream out;
        out << "{\"data\":{\"setComponent\":";
        append_component_instance(out, entity, component);
        out << "}}";
        return out.str();
    }

    void append_component_instance(std::ostringstream& out, Entity entity, Entity component) {
        const std::vector<EntityComponentInfo> singleton_components = registry_.singleton_components();
        const auto singleton = std::find_if(
            singleton_components.begin(),
            singleton_components.end(),
            [&](const EntityComponentInfo& candidate) {
                return candidate.component == component;
            });
        if (singleton != singleton_components.end()) {
            (void)entity;
            append_component(out, registry_.singleton_storage_entity(), *singleton);
            return;
        }
        const std::vector<EntityComponentInfo> components = registry_.components(entity);
        const auto found = std::find_if(components.begin(), components.end(), [&](const EntityComponentInfo& info) {
            return info.component == component;
        });
        if (found == components.end()) {
            out << "null";
            return;
        }
        append_component(out, entity, *found);
    }

    void append_entity_detail(std::ostringstream& out, Entity entity) {
        const std::string display_name = debug_name_display_name(entity);
        out << "{\"id\":" << quoted(entity_id(entity))
            << ",\"index\":" << Registry::entity_index(entity)
            << ",\"version\":" << Registry::entity_version(entity)
            << ",\"kind\":" << quoted(kind_name(registry_.entity_kind(entity)));
        if (!display_name.empty()) {
            out << ",\"displayName\":" << quoted(display_name);
        }
        out << ",\"components\":[";
        const std::vector<EntityComponentInfo> components = registry_.components(entity);
        for (std::size_t i = 0; i < components.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_component(out, entity, components[i]);
        }
        out << "],\"matchingJobs\":[";
        bool first_job = true;
        registry_.view<>().with_tags({registry_.job_tag()}).each([&](Entity job) {
            const std::vector<Entity> matches = registry_.job_matching_entities(job);
            if (std::find(matches.begin(), matches.end(), entity) == matches.end()) {
                return;
            }
            const std::optional<JobInfo> info = registry_.job_info(job);
            if (!info) {
                return;
            }
            if (!first_job) {
                out << ",";
            }
            first_job = false;
            append_job(out, *info);
        });
        out << "]}";
    }

    void append_component(std::ostringstream& out, Entity entity, const EntityComponentInfo& component) {
        out << "{\"component\":" << quoted(entity_id(component.component))
            << ",\"name\":" << quoted(component.name)
            << ",\"tag\":" << (component.info.tag ? "true" : "false")
            << ",\"singleton\":" << (component.singleton ? "true" : "false")
            << ",\"dirty\":" << (component.dirty ? "true" : "false")
            << ",\"debugValue\":" << quoted(component.debug_value)
            << ",\"fields\":[";
        const std::vector<ComponentField>* fields = registry_.component_fields(component.component);
        const unsigned char* bytes = component.info.tag
            ? nullptr
            : reinterpret_cast<const unsigned char*>(registry_.get(entity, component.component));
        bool first = true;
        if (fields != nullptr && bytes != nullptr) {
            for (const ComponentField& field : *fields) {
                const char* type = editable_field_type(registry_, field);
                if (type == nullptr) {
                    continue;
                }
                if (!first) {
                    out << ",";
                }
                first = false;
                out << "{\"name\":" << quoted(field.name)
                    << ",\"type\":" << quoted(type)
                    << ",\"value\":";
                append_field_value(out, registry_, field, bytes);
                out << "}";
            }
        }
        out << "]}";
    }

    void append_job(std::ostringstream& out, const JobInfo& job) const {
        out << "{\"id\":" << quoted(entity_id(job.entity))
            << ",\"name\":" << quoted(job.name)
            << ",\"order\":" << job.order
            << ",\"structural\":" << (job.structural ? "true" : "false")
            << ",\"singleThread\":" << (job.single_thread ? "true" : "false")
            << ",\"maxThreads\":" << job.max_threads
            << ",\"minEntitiesPerThread\":" << job.min_entities_per_thread
            << ",\"reads\":[";
        for (std::size_t i = 0; i < job.reads.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_component_ref(out, registry_, job.reads[i]);
        }
        out << "],\"writes\":[";
        for (std::size_t i = 0; i < job.writes.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_component_ref(out, registry_, job.writes[i]);
        }
        out << "]}";
    }

    void append_job_detail(std::ostringstream& out, const JobInfo& job) {
        out << "{\"id\":" << quoted(entity_id(job.entity))
            << ",\"name\":" << quoted(job.name)
            << ",\"order\":" << job.order
            << ",\"structural\":" << (job.structural ? "true" : "false")
            << ",\"singleThread\":" << (job.single_thread ? "true" : "false")
            << ",\"maxThreads\":" << job.max_threads
            << ",\"minEntitiesPerThread\":" << job.min_entities_per_thread
            << ",\"reads\":[";
        for (std::size_t i = 0; i < job.reads.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_component_ref(out, registry_, job.reads[i]);
        }
        out << "],\"writes\":[";
        for (std::size_t i = 0; i < job.writes.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_component_ref(out, registry_, job.writes[i]);
        }
        out << "],\"matchingEntities\":[";
        const std::vector<Entity> matches = registry_.job_matching_entities(job.entity);
        const std::map<std::uint64_t, std::string> display_names = entity_display_names();
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            const auto display_name = display_names.find(matches[i].value);
            append_entity(
                out,
                registry_,
                matches[i],
                display_name != display_names.end() ? display_name->second : std::string{});
        }
        out << "]}";
    }

    std::string debug_name_display_name(Entity entity) const {
        const std::string name = debug_name(entity);
        if (name.empty()) {
            return {};
        }
        std::size_t total = 0;
        std::size_t ordinal = 0;
        registry_.view<>().each([&](Entity current) {
            if (debug_name(current) != name) {
                return;
            }
            ++total;
            if (current.value <= entity.value) {
                ++ordinal;
            }
        });
        return total > 1 ? name + " " + std::to_string(ordinal) : name;
    }

    std::string debug_name(Entity entity) const {
        if (!debug_name_component_) {
            return {};
        }
        const auto* name = static_cast<const DebugName*>(registry_.get(entity, debug_name_component_));
        return name != nullptr ? name->str() : std::string{};
    }

    std::map<std::uint64_t, std::string> entity_display_names() {
        std::map<std::string, std::size_t> totals;
        registry_.view<>().each([&](Entity entity) {
            const std::string name = debug_name(entity);
            if (!name.empty()) {
                ++totals[name];
            }
        });

        std::map<std::string, std::size_t> ordinals;
        std::map<std::uint64_t, std::string> display_names;
        registry_.view<>().each([&](Entity entity) {
            const std::string name = debug_name(entity);
            if (name.empty()) {
                return;
            }
            if (totals[name] <= 1) {
                display_names.emplace(entity.value, name);
                return;
            }
            const std::size_t ordinal = ++ordinals[name];
            display_names.emplace(entity.value, name + " " + std::to_string(ordinal));
        });
        return display_names;
    }

    Registry& registry_;
    DebugServerOptions options_;
    Entity debug_name_component_;
    Socket listen_socket_ = invalid_socket;
    std::vector<Client> clients_;
    bool active_ = false;
    std::uint16_t bound_port_ = 0;
    std::string last_error_;
};

DebugServer::DebugServer(Registry& registry, DebugServerOptions options)
    : impl_(std::make_unique<Impl>(registry, std::move(options))) {}

DebugServer::DebugServer(Registry& registry, std::string name, DebugServerOptions options) {
    options.name = std::move(name);
    impl_ = std::make_unique<Impl>(registry, std::move(options));
}

DebugServer::DebugServer(DebugServer&&) noexcept = default;
DebugServer& DebugServer::operator=(DebugServer&&) noexcept = default;
DebugServer::~DebugServer() = default;

void DebugServer::poll() {
    impl_->poll();
}

bool DebugServer::enabled() const noexcept {
    return impl_ != nullptr && impl_->enabled();
}

std::uint16_t DebugServer::port() const noexcept {
    return impl_ != nullptr ? impl_->port() : 0;
}

const std::string& DebugServer::last_error() const noexcept {
    static const std::string empty;
    return impl_ != nullptr ? impl_->last_error() : empty;
}

}  // namespace ecs

#endif
