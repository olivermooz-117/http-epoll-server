#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

constexpr int PORT = 8080;
constexpr int MAX_EVENTS = 1024;
constexpr int BACKLOG = 128;

constexpr size_t MAX_HEADER_SIZE = 16384;
constexpr size_t MAX_BUFFER_SIZE = 1024 * 1024;

constexpr int KEEP_ALIVE_TIMEOUT_SECONDS = 30;
constexpr int MAX_REQUESTS_PER_CONNECTION = 100;
constexpr int EPOLL_WAIT_TIMEOUT_MS = 1000;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

const fs::path WWW_ROOT = "www";

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    bool keep_alive = false;
};

struct Connection {
    int fd;

    std::string read_buf;
    std::string write_buf;

    size_t write_offset = 0;
    size_t request_count = 0;

    bool response_ready = false;
    bool keep_alive = false;
    bool peer_closed = false;

    Clock::time_point last_activity = Clock::now();

    explicit Connection(int socket_fd) : fd(socket_fd) {}
};

enum class ParseResult {
    INCOMPLETE,
    INVALID,
    VALID
};

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) return false;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

int create_server_socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    if (!set_nonblocking(fd)) {
        perror("set_nonblocking");
        close(fd);
        return -1;
    }

    return fd;
}

std::string trim(const std::string& value) {
    size_t start = value.find_first_not_of(" \t");

    if (start == std::string::npos) return "";

    size_t end = value.find_last_not_of(" \t");

    return value.substr(start, end - start + 1);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

    return value;
}

bool valid_header_name(const std::string& name) {
    if (name.empty()) return false;

    const std::string allowed = "!#$%&'*+-.^_`|~";

    for (unsigned char c : name) {
        if (!std::isalnum(c) && allowed.find(c) == std::string::npos) {
            return false;
        }
    }

    return true;
}

bool contains_token(const std::string& value,
                    const std::string& token) {
    std::istringstream stream(value);
    std::string item;

    while (std::getline(stream, item, ',')) {
        if (lowercase(trim(item)) == lowercase(token)) {
            return true;
        }
    }

    return false;
}

ParseResult parse_request(
    const std::string& buffer,
    HttpRequest& req,
    size_t& consumed,
    int& error_status
) {
    size_t header_end = buffer.find("\r\n\r\n");

    if (header_end == std::string::npos) {
        if (buffer.size() > MAX_HEADER_SIZE) {
            error_status = 431;
            return ParseResult::INVALID;
        }

        return ParseResult::INCOMPLETE;
    }

    if (header_end > MAX_HEADER_SIZE) {
        error_status = 431;
        return ParseResult::INVALID;
    }

    consumed = header_end + 4;

    std::istringstream stream(buffer.substr(0, header_end));

    std::string request_line;

    if (!std::getline(stream, request_line)) {
        error_status = 400;
        return ParseResult::INVALID;
    }

    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }

    std::istringstream line(request_line);
    std::string extra;

    if (!(line >> req.method >> req.path >> req.version) ||
        (line >> extra)) {
        error_status = 400;
        return ParseResult::INVALID;
    }

    if (req.version != "HTTP/1.0" &&
        req.version != "HTTP/1.1") {
        error_status = 505;
        return ParseResult::INVALID;
    }

    std::string header;

    while (std::getline(stream, header)) {
        if (!header.empty() && header.back() == '\r') {
            header.pop_back();
        }

        if (header.empty()) continue;

        if (header[0] == ' ' || header[0] == '\t') {
            error_status = 400;
            return ParseResult::INVALID;
        }

        size_t colon = header.find(':');

        if (colon == std::string::npos) {
            error_status = 400;
            return ParseResult::INVALID;
        }

        std::string name = header.substr(0, colon);
        std::string value = trim(header.substr(colon + 1));

        if (!valid_header_name(name)) {
            error_status = 400;
            return ParseResult::INVALID;
        }

        name = lowercase(name);

        if (req.headers.count(name)) {
            error_status = 400;
            return ParseResult::INVALID;
        }

        req.headers[name] = value;
    }

    if (req.version == "HTTP/1.1" &&
        !req.headers.count("host")) {
        error_status = 400;
        return ParseResult::INVALID;
    }

    if (req.headers.count("transfer-encoding")) {
        error_status = 501;
        return ParseResult::INVALID;
    }

    if (req.headers.count("content-length")) {
        std::string length = req.headers["content-length"];

        if (length != "0") {
            error_status = 400;
            return ParseResult::INVALID;
        }
    }

    req.keep_alive = (req.version == "HTTP/1.1");

    if (req.headers.count("connection")) {
        std::string connection = req.headers["connection"];

        if (contains_token(connection, "close")) {
            req.keep_alive = false;
        } else if (contains_token(connection, "keep-alive")) {
            req.keep_alive = true;
        }
    }

    if (req.method != "GET" && req.method != "HEAD") {
        error_status = 405;
        return ParseResult::INVALID;
    }

    if (req.path.empty() || req.path[0] != '/') {
        error_status = 400;
        return ParseResult::INVALID;
    }

    return ParseResult::VALID;
}

std::string status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

std::string content_type(const std::string& path) {
    std::string ext = fs::path(path).extension().string();

    ext = lowercase(ext);

    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".pdf") return "application/pdf";

    return "application/octet-stream";
}

bool decode_url_path(
    const std::string& input,
    std::string& output
) {
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = input[i];

        if (c == '%') {
            if (i + 2 >= input.size()) return false;

            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };

            int hi = hex(input[i + 1]);
            int lo = hex(input[i + 2]);

            if (hi < 0 || lo < 0) return false;

            c = static_cast<unsigned char>((hi << 4) | lo);
            i += 2;
        }

        if (c == '\0' || c == '\\' || c < 32 || c == 127) {
            return false;
        }

        output.push_back(static_cast<char>(c));
    }

    return true;
}

bool is_inside_root(
    const fs::path& root,
    const fs::path& target
) {
    auto r = root.begin();
    auto t = target.begin();

    for (; r != root.end(); ++r, ++t) {
        if (t == target.end() || *r != *t) {
            return false;
        }
    }

    return true;
}

bool load_file(
    const std::string& url_path,
    std::string& body,
    std::string& resolved_path,
    int& status
) {
    std::string path_only = url_path.substr(
        0, url_path.find('?')
    );

    if (path_only.find('#') != std::string::npos) {
        status = 400;
        return false;
    }

    std::string decoded;

    if (!decode_url_path(path_only, decoded)) {
        status = 400;
        return false;
    }

    if (decoded == "/") {
        decoded = "/index.html";
    }

    fs::path relative = fs::path(decoded).relative_path();

    for (const auto& component : relative) {
        if (component == "..") {
            status = 400;
            return false;
        }
    }

    try {
        fs::path root = fs::canonical(WWW_ROOT);
        fs::path target = fs::weakly_canonical(root / relative);

        if (!is_inside_root(root, target)) {
            status = 400;
            return false;
        }

        if (!fs::is_regular_file(target)) {
            status = 404;
            return false;
        }

        if (fs::file_size(target) > 10 * 1024 * 1024) {
            status = 413;
            return false;
        }

        int file_fd = open(target.c_str(), O_RDONLY | O_CLOEXEC);

        if (file_fd < 0) {
            status = (errno == ENOENT || errno == EACCES)
                ? 404 : 500;
            return false;
        }

        struct stat file_stat{};

        if (fstat(file_fd, &file_stat) < 0 ||
            !S_ISREG(file_stat.st_mode) ||
            file_stat.st_size < 0 ||
            file_stat.st_size > 10 * 1024 * 1024) {
            close(file_fd);
            status = 404;
            return false;
        }

        body.clear();

        char buffer[8192];

        while (true) {
            ssize_t n = read(file_fd, buffer, sizeof(buffer));

            if (n > 0) {
                body.append(buffer, static_cast<size_t>(n));
            } else if (n == 0) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                close(file_fd);
                status = 500;
                return false;
            }
        }

        close(file_fd);

        resolved_path = target.string();

        return true;

    } catch (const fs::filesystem_error&) {
        status = 404;
        return false;
    }
}

std::string make_response(
    int status,
    const std::string& type,
    const std::string& body,
    bool keep_alive,
    bool head_request
) {
    std::ostringstream response;

    response << "HTTP/1.1 " << status << " "
             << status_text(status) << "\r\n";

    response << "Content-Type: " << type << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";

    if (status == 405) {
        response << "Allow: GET, HEAD\r\n";
    }

    response << "Connection: "
             << (keep_alive ? "keep-alive" : "close")
             << "\r\n";

    if (keep_alive) {
        response << "Keep-Alive: timeout="
                 << KEEP_ALIVE_TIMEOUT_SECONDS
                 << ", max="
                 << MAX_REQUESTS_PER_CONNECTION
                 << "\r\n";
    }

    response << "Server: epoll-http/0.4\r\n";
    response << "\r\n";

    if (!head_request) {
        response << body;
    }

    return response.str();
}

void prepare_response(Connection& conn) {
    if (conn.response_ready) return;

    HttpRequest req;
    size_t consumed = 0;
    int error_status = 400;

    ParseResult result = parse_request(
        conn.read_buf, req, consumed, error_status
    );

    if (result == ParseResult::INCOMPLETE) {
        return;
    }

    if (result == ParseResult::INVALID) {
        std::string body = status_text(error_status) + "\n";

        conn.write_buf = make_response(
            error_status,
            "text/plain; charset=utf-8",
            body,
            false,
            false
        );

        conn.keep_alive = false;
    } else {
        conn.read_buf.erase(0, consumed);

        ++conn.request_count;

        std::string body;
        std::string resolved_path;
        int status = 200;

        bool success = load_file(
            req.path, body, resolved_path, status
        );

        if (!success) {
            body = status_text(status) + "\n";
        }

        conn.keep_alive =
            req.keep_alive &&
            conn.request_count < MAX_REQUESTS_PER_CONNECTION;

        conn.write_buf = make_response(
            success ? 200 : status,
            success ? content_type(resolved_path)
                    : "text/plain; charset=utf-8",
            body,
            conn.keep_alive,
            req.method == "HEAD"
        );

        std::cout << req.method << " " << req.path
                  << " -> " << (success ? 200 : status)
                  << " [request " << conn.request_count << "]"
                  << (conn.keep_alive ? " [keep-alive]" : " [close]")
                  << "\n";
    }

    conn.write_offset = 0;
    conn.response_ready = true;
}

void close_connection(
    int epfd,
    int fd,
    std::unordered_map<int, std::unique_ptr<Connection>>& connections
) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    connections.erase(fd);
}

void cleanup_idle_connections(
    int epfd,
    std::unordered_map<int, std::unique_ptr<Connection>>& connections
) {
    const auto now = Clock::now();

    std::vector<int> expired;

    for (const auto& item : connections) {
        const Connection& conn = *item.second;

        if (now - conn.last_activity >=
            std::chrono::seconds(KEEP_ALIVE_TIMEOUT_SECONDS)) {
            expired.push_back(conn.fd);
        }
    }

    for (int fd : expired) {
        std::cout << "Connection timed out: " << fd << "\n";
        close_connection(epfd, fd, connections);
    }
}

void update_events(int epfd, Connection& conn) {
    epoll_event ev{};

    ev.events = EPOLLIN | EPOLLRDHUP;

    if (conn.response_ready) {
        ev.events |= EPOLLOUT;
    }

    ev.data.fd = conn.fd;

    epoll_ctl(epfd, EPOLL_CTL_MOD, conn.fd, &ev);
}

int main() {
    int listen_fd = create_server_socket();

    if (listen_fd < 0) return 1;

    int epfd = epoll_create1(0);

    if (epfd < 0) {
        perror("epoll_create1");
        close(listen_fd);
        return 1;
    }

    epoll_event listen_event{};

    listen_event.events = EPOLLIN;
    listen_event.data.fd = listen_fd;

    if (epoll_ctl(
        epfd, EPOLL_CTL_ADD, listen_fd, &listen_event
    ) < 0) {
        perror("epoll_ctl");
        close(epfd);
        close(listen_fd);
        return 1;
    }

    std::unordered_map<int, std::unique_ptr<Connection>> connections;

    epoll_event events[MAX_EVENTS];

    std::cout << "====================================\n";
    std::cout << "     EPOLL HTTP SERVER v0.4\n";
    std::cout << "====================================\n";
    std::cout << "Listening: http://127.0.0.1:"
              << PORT << "\n";
    std::cout << "Document root: " << WWW_ROOT << "\n";
    std::cout << "Keep-Alive timeout: "
              << KEEP_ALIVE_TIMEOUT_SECONDS << " seconds\n";
    std::cout << "Maximum requests per connection: "
              << MAX_REQUESTS_PER_CONNECTION << "\n";
    std::cout << "Methods: GET, HEAD\n";
    std::cout << "====================================\n";

    while (true) {
        int n = epoll_wait(
            epfd,
            events,
            MAX_EVENTS,
            EPOLL_WAIT_TIMEOUT_MS
        );

        if (n < 0) {
            if (errno == EINTR) continue;

            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t flags = events[i].events;

            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);

                    int client_fd = accept(
                        listen_fd,
                        reinterpret_cast<sockaddr*>(&client_addr),
                        &client_len
                    );

                    if (client_fd < 0) {
                        if (errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                            break;
                        }

                        if (errno == EINTR) continue;

                        perror("accept");
                        break;
                    }

                    if (!set_nonblocking(client_fd)) {
                        close(client_fd);
                        continue;
                    }

                    epoll_event client_event{};

                    client_event.events = EPOLLIN | EPOLLRDHUP;
                    client_event.data.fd = client_fd;

                    if (epoll_ctl(
                        epfd, EPOLL_CTL_ADD,
                        client_fd, &client_event
                    ) < 0) {
                        close(client_fd);
                        continue;
                    }

                    connections[client_fd] =
                        std::make_unique<Connection>(client_fd);

                    std::cout << "Client connected: "
                              << client_fd << "\n";
                }

                continue;
            }

            auto it = connections.find(fd);

            if (it == connections.end()) continue;

            Connection& conn = *it->second;

            if (flags & (EPOLLERR | EPOLLHUP)) {
                close_connection(epfd, fd, connections);
                continue;
            }

            if (flags & EPOLLRDHUP) {
                conn.peer_closed = true;
            }

            bool failed = false;

            if (flags & EPOLLIN) {
                char buffer[8192];

                while (true) {
                    ssize_t bytes = recv(
                        fd, buffer, sizeof(buffer), 0
                    );

                    if (bytes > 0) {
                        conn.last_activity = Clock::now();

                        conn.read_buf.append(
                            buffer, static_cast<size_t>(bytes)
                        );

                        if (conn.read_buf.size() > MAX_BUFFER_SIZE) {
                            failed = true;
                            break;
                        }
                    } else if (bytes == 0) {
                        conn.peer_closed = true;
                        break;
                    } else {
                        if (errno == EINTR) continue;

                        if (errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                            break;
                        }

                        failed = true;
                        break;
                    }
                }
            }

            if (failed) {
                close_connection(epfd, fd, connections);
                continue;
            }

            if (!conn.response_ready && !conn.read_buf.empty()) {
                prepare_response(conn);
            }

            if (conn.response_ready) {
                update_events(epfd, conn);
            }

            if ((flags & EPOLLOUT) && conn.response_ready) {
                while (conn.write_offset < conn.write_buf.size()) {
                    ssize_t bytes = send(
                        fd,
                        conn.write_buf.data() + conn.write_offset,
                        conn.write_buf.size() - conn.write_offset,
                        MSG_NOSIGNAL
                    );

                    if (bytes > 0) {
                        conn.write_offset +=
                            static_cast<size_t>(bytes);

                        conn.last_activity = Clock::now();
                    } else if (bytes < 0 && errno == EINTR) {
                        continue;
                    } else if (bytes < 0 &&
                               (errno == EAGAIN ||
                                errno == EWOULDBLOCK)) {
                        break;
                    } else {
                        failed = true;
                        break;
                    }
                }
            }

            if (failed) {
                close_connection(epfd, fd, connections);
                continue;
            }

            if (conn.response_ready &&
                conn.write_offset == conn.write_buf.size()) {

                if (!conn.keep_alive || conn.peer_closed) {
                    close_connection(epfd, fd, connections);
                    continue;
                }

                conn.write_buf.clear();
                conn.write_offset = 0;
                conn.response_ready = false;

                conn.last_activity = Clock::now();

                if (!conn.read_buf.empty()) {
                    prepare_response(conn);
                }

                update_events(epfd, conn);
            }
        }

        cleanup_idle_connections(epfd, connections);
    }

    for (auto& item : connections) {
        close(item.first);
    }

    close(epfd);
    close(listen_fd);

    return 0;
}
