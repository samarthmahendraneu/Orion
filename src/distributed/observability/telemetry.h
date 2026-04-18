//
// telemetry.h — Minimalist OpenTelemetry tracing and lineage for Orion.
//
#pragma once

#include <chrono>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../generated/orion.pb.h"

namespace orion::observability {

// Active trace ID for the current thread (picked up by the JSON logger)
inline thread_local std::string g_current_trace_id;

struct TraceContext {
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;

    bool is_valid() const { return !trace_id.empty() && !span_id.empty(); }

    // Convert to/from proto message
    void to_proto(orion::TraceContext* proto) const {
        proto->set_trace_id(trace_id);
        proto->set_span_id(span_id);
        proto->set_parent_span_id(parent_span_id);
    }
    static TraceContext from_proto(const orion::TraceContext& proto) {
        return {proto.trace_id(), proto.span_id(), proto.parent_span_id()};
    }
};

struct SpanData {
    std::string name;
    TraceContext ctx;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    std::map<std::string, std::string> attributes;
    std::string component;
};

class Span {
public:
    Span(std::string name, TraceContext ctx, std::string component)
        : name_(std::move(name)), component_(std::move(component)) {
        data_.name = name_;
        data_.component = component_;
        data_.ctx = std::move(ctx);
        data_.start_time = std::chrono::system_clock::now();
    }

    void set_attribute(std::string key, std::string value) {
        std::lock_guard<std::mutex> lock(mu_);
        data_.attributes[std::move(key)] = std::move(value);
    }

    void end();

    const TraceContext& context() const { return data_.ctx; }
    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::string component_;
    SpanData data_;
    std::mutex mu_;
    bool ended_ = false;
};

class Tracer {
public:
    static Tracer& instance() {
        static Tracer t;
        return t;
    }

    // Start a root span or a child of an existing context
    std::shared_ptr<Span> start_span(const std::string& name, 
                                     const std::string& component,
                                     const TraceContext* parent = nullptr) {
        TraceContext ctx;
        if (parent && parent->is_valid()) {
            ctx.trace_id = parent->trace_id;
            ctx.parent_span_id = parent->span_id;
        } else {
            ctx.trace_id = generate_id(32); // 128-bit trace ID
        }
        ctx.span_id = generate_id(16); // 64-bit span ID

        auto span = std::make_shared<Span>(name, std::move(ctx), component);
        return span;
    }

    // ID generation helper
    static std::string generate_id(int length) {
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<> dis(0, 15);
        std::stringstream ss;
        if (length == 32) ss << "tr-";
        else if (length == 16) ss << "sp-";
        
        for (int i = 0; i < length; ++i) ss << std::hex << dis(gen);
        return ss.str();
    }

    // Callback for exported spans (wired to OTLP exporter later)
    using ExportCallback = std::function<void(const SpanData&)>;
    void set_export_callback(ExportCallback cb) {
        std::lock_guard<std::mutex> lock(mu_);
        export_cb_ = std::move(cb);
    }

    void export_span(const SpanData& data) {
        std::lock_guard<std::mutex> lock(mu_);
        if (export_cb_) export_cb_(data);
    }

private:
    Tracer() = default;
    std::mutex mu_;
    ExportCallback export_cb_;
};

inline void Span::end() {
    std::lock_guard<std::mutex> lock(mu_);
    if (ended_) return;
    data_.end_time = std::chrono::system_clock::now();
    ended_ = true;
    Tracer::instance().export_span(data_);
}

} // namespace orion::observability
