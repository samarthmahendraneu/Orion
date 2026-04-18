//
// http_server.h — A ~200-line HTTP/1.1 server for Prometheus scrapes and
// cluster introspection. No external deps; raw BSD sockets only.
//
// Scope is deliberately tiny:
//   - GET-only
//   - request lines up to 4 KB
//   - one thread per connection (fine: scrapers poll once every 15s)
//   - no keep-alive (`Connection: close` on every response)
//   - routes are exact matches on the path; no query-string parsing
//
// It exists so you can `curl localhost:9090/metrics` during an incident
// without shipping a separate Prometheus sidecar.
//
// Routes (wired by HeadMetricsServer):
//   GET /metrics  -> Prometheus text format
//   GET /cluster  -> JSON status (counters + node list + in-flight count)
//   GET /healthz  -> 200 OK / literal "ok"
//
// NOT production-grade for public exposure — bind to localhost or lock it
// behind a private network. This is observability plumbing, not a REST API.
//

#pragma once

#include <arpa/inet.h>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace orion::observability {

class HttpServer {
public:
    using Handler = std::function<std::string(void)>;

    explicit HttpServer(int port) : port_(port) {}

    ~HttpServer() { stop(); }

    void route(const std::string& path, Handler h) { routes_[path] = std::move(h); }

    // Start listening; returns false if bind fails.
    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port_));

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (::listen(listen_fd_, 16) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop_(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    int port() const { return port_; }

private:
    void accept_loop_() {
        while (running_) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &len);
            if (cfd < 0) {
                if (!running_) break;
                continue;
            }
            // detached thread per connection — scrapes are rare enough that
            // a thread pool is overkill
            std::thread([this, cfd] { handle_conn_(cfd); }).detach();
        }
    }

    void handle_conn_(int fd) {
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { ::close(fd); return; }
        buf[n] = '\0';

        // Extract the path — "GET /metrics HTTP/1.1\r\n..."
        std::string req(buf, static_cast<size_t>(n));
        auto first_sp = req.find(' ');
        auto second_sp = (first_sp == std::string::npos)
            ? std::string::npos
            : req.find(' ', first_sp + 1);

        std::string method = (first_sp == std::string::npos)
            ? "" : req.substr(0, first_sp);
        std::string path = (second_sp == std::string::npos)
            ? "/" : req.substr(first_sp + 1, second_sp - first_sp - 1);

        if (method != "GET") {
            write_response_(fd, 405, "text/plain", "method not allowed");
            ::close(fd);
            return;
        }

        auto it = routes_.find(path);
        if (it == routes_.end()) {
            write_response_(fd, 404, "text/plain", "not found");
            ::close(fd);
            return;
        }

        std::string body;
        try {
            body = it->second();
        } catch (const std::exception& e) {
            write_response_(fd, 500, "text/plain", std::string("handler threw: ") + e.what());
            ::close(fd);
            return;
        }

        // Content-type heuristic.
        std::string ctype = "text/plain; version=0.0.4";  // prom default
        if (!body.empty() && body.front() == '{') {
            ctype = "application/json";
        }
        write_response_(fd, 200, ctype, body);
        ::close(fd);
    }

    static void write_response_(int fd, int status, const std::string& ctype,
                                const std::string& body) {
        const char* reason =
            (status == 200) ? "OK" :
            (status == 404) ? "Not Found" :
            (status == 405) ? "Method Not Allowed" :
            (status == 500) ? "Internal Server Error" : "Unknown";
        std::string resp;
        resp.reserve(body.size() + 128);
        resp += "HTTP/1.1 ";
        resp += std::to_string(status);
        resp += ' ';
        resp += reason;
        resp += "\r\nConnection: close\r\nContent-Type: ";
        resp += ctype;
        resp += "\r\nContent-Length: ";
        resp += std::to_string(body.size());
        resp += "\r\n\r\n";
        resp += body;
        ::send(fd, resp.data(), resp.size(), 0);
    }

    int port_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::unordered_map<std::string, Handler> routes_;
};

} // namespace orion::observability
