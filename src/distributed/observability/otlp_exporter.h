//
// otlp_exporter.h — Minimalist OTLP/HTTP JSON exporter for Orion.
//
#pragma once

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <queue>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include "telemetry.h"
#include "logger.h"

namespace orion::observability {

class OtlpExporter {
public:
    explicit OtlpExporter(std::string host, int port)
        : host_(std::move(host)), port_(port) {
        
        // Wire up the Tracer to us.
        Tracer::instance().set_export_callback([this](const SpanData& data) {
            this->enqueue(data);
        });

        worker_ = std::thread([this] { worker_loop(); });
    }

    ~OtlpExporter() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    void enqueue(const SpanData& data) {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push(data);
        cv_.notify_one();
    }

    void worker_loop() {
        while (true) {
            std::vector<SpanData> batch;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) break;

                while (!queue_.empty() && batch.size() < 10) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop();
                }
            }
            if (!batch.empty()) {
                send_batch(batch);
            }
        }
    }

    void send_batch(const std::vector<SpanData>& batch) {
        std::string json = format_otlp_json(batch);
        
        // Simple raw-socket POST.
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;

        struct timeval tv;
        tv.tv_sec = 2; tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        
        struct hostent* he = gethostbyname(host_.c_str());
        if (!he) { ::close(fd); return; }
        ::memcpy(&addr.sin_addr, he->h_addr, static_cast<size_t>(he->h_length));

        if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd);
            return;
        }

        std::ostringstream ss;
        ss << "POST /v1/traces HTTP/1.1\r\n"
           << "Host: " << host_ << "\r\n"
           << "Content-Type: application/json\r\n"
           << "Content-Length: " << json.size() << "\r\n"
           << "Connection: close\r\n\r\n"
           << json;
        
        std::string req = ss.str();
        ::send(fd, req.data(), req.size(), 0);
        
        // Wait for a small reply or timeout
        char buf[256];
        ::recv(fd, buf, sizeof(buf), 0);
        
        ::close(fd);
    }

    std::string format_otlp_json(const std::vector<SpanData>& batch) {
        std::ostringstream os;
        os << "{\"resourceSpans\": [{ \"resource\": { \"attributes\": ["
           << "{\"key\": \"service.name\", \"value\": {\"stringValue\": \"orion\"}}"
           << "]}, \"scopeSpans\": [{ \"spans\": [";

        for (size_t i = 0; i < batch.size(); ++i) {
            const auto& s = batch[i];
            if (i > 0) os << ",";
            os << "{"
               << "\"traceId\": \"" << s.ctx.trace_id << "\","
               << "\"spanId\": \"" << s.ctx.span_id << "\","
               << (s.ctx.parent_span_id.empty() ? "" : ("\"parentSpanId\": \"" + s.ctx.parent_span_id + "\","))
               << "\"name\": \"" << s.name << "\","
               << "\"kind\": 1,"
               << "\"startTimeUnixNano\": \"" << to_nano(s.start_time) << "\","
               << "\"endTimeUnixNano\": \"" << to_nano(s.end_time) << "\","
               << "\"attributes\": [";
            
            bool first_attr = true;
            for (const auto& [k, v] : s.attributes) {
                if (!first_attr) os << ",";
                os << "{\"key\": \"" << k << "\", \"value\": {\"stringValue\": \"" << v << "\"}}";
                first_attr = false;
            }
            if (!first_attr) os << ",";
            os << "{\"key\": \"component\", \"value\": {\"stringValue\": \"" << s.component << "\"}}";
            
            os << "]}";
        }

        os << "]}]}]}";
        return os.str();
    }

    static uint64_t to_nano(std::chrono::system_clock::time_point tp) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    }

    std::string host_;
    int port_;
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<SpanData> queue_;
    bool stop_ = false;
};

} // namespace orion::observability
