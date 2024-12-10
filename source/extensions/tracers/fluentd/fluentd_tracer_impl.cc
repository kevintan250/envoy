#include "source/extensions/tracers/fluentd/fluentd_tracer_impl.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/backoff_strategy.h"
#include "source/common/tracing/trace_context_impl.h"
#include "source/common/common/hex.h"

#include "msgpack.hpp"
#include <cstdint>

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace Fluentd {

using MessagePackBuffer = msgpack::sbuffer;
using MessagePackPacker = msgpack::packer<msgpack::sbuffer>;

Driver::Driver(const FluentdConfigSharedPtr fluentd_config,
               Server::Configuration::TracerFactoryContext& context, FluentdTracerCacheSharedPtr tracer_cache)
    : tls_slot_(context.serverFactoryContext().threadLocal().allocateSlot()), fluentd_config_(fluentd_config), tracer_cache_(tracer_cache) {
      Random::RandomGenerator& random = context.serverFactoryContext().api().randomGenerator();

  tls_slot_->set(
      [fluentd_config = fluentd_config_, &random, tracer_cache = tracer_cache_](Event::Dispatcher&) {
        return std::make_shared<ThreadLocalTracer>(
            tracer_cache->getOrCreateTracer(fluentd_config, random));
      });
}

Tracing::SpanPtr Driver::startSpan(const Tracing::Config& config,
                                   Tracing::TraceContext& trace_context,
                                   const StreamInfo::StreamInfo& stream_info,
                                   const std::string& operation_name,
                                   Tracing::Decision tracing_decision) {
  auto& tracer = tls_slot_->getTyped<ThreadLocalTracer>().tracer();

  return tracer.startSpan(config, trace_context, stream_info, operation_name, tracing_decision);
}

FluentdTracerImpl::FluentdTracerImpl(Upstream::ThreadLocalCluster& cluster,
                                     Tcp::AsyncTcpClientPtr client, Event::Dispatcher& dispatcher,
                                     const FluentdConfig& config,
                                     BackOffStrategyPtr backoff_strategy,
                                     Stats::Scope& parent_scope,
                                     Random::RandomGenerator& random)
    : tag_(config.tag()), id_(dispatcher.name()),
      max_connect_attempts_(
          config.has_retry_options() && config.retry_options().has_max_connect_attempts()
              ? absl::optional<uint32_t>(config.retry_options().max_connect_attempts().value())
              : absl::nullopt),
      stats_scope_(parent_scope.createScope(config.stat_prefix())),
      fluentd_stats_(
          {TRACER_FLUENTD_STATS(POOL_COUNTER(*stats_scope_), POOL_GAUGE(*stats_scope_))}),
      cluster_(cluster), backoff_strategy_(std::move(backoff_strategy)), client_(std::move(client)),
      buffer_flush_interval_msec_(
          PROTOBUF_GET_MS_OR_DEFAULT(config, buffer_flush_interval, DefaultBufferFlushIntervalMs)),
      max_buffer_size_bytes_(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, buffer_size_bytes, DefaultMaxBufferSize)),
      retry_timer_(dispatcher.createTimer([this]() -> void { onBackoffCallback(); })),
      flush_timer_(dispatcher.createTimer([this]() {
        ENVOY_LOG(info, "Flushing buffer due to timeout, entries: {}", entries_.size());
        flush();
        flush_timer_->enableTimer(buffer_flush_interval_msec_);
      })),
      option_({{"fluent_signal", "2"}, {"TimeFormat", "DateTime"}}), random_(random) {

  client_->setAsyncTcpClientCallbacks(*this);
  flush_timer_->enableTimer(buffer_flush_interval_msec_);

  ENVOY_LOG(info, "Fluentd tracer initialized with buffer size {} bytes, flush interval {} ms",
            max_buffer_size_bytes_, buffer_flush_interval_msec_.count());
}

// make a span object
Span::Span(const Tracing::Config& config, Tracing::TraceContext& trace_context,
           const StreamInfo::StreamInfo& stream_info, const std::string& operation_name,
           Tracing::Decision tracing_decision, FluentdTracerSharedPtr tracer, const SpanContext& span_context)
    : trace_context_(trace_context), stream_info_(stream_info), operation_(operation_name),
      tracing_decision_(tracing_decision), tracer_(tracer), span_context_(span_context) {}


void Span::setOperation(absl::string_view operation) { 
  operation_ = std::string(operation); 
}

void Span::setTag(absl::string_view name, absl::string_view value) {
  tags_[std::string(name)] = std::string(value);
}

void Span::log(SystemTime timestamp, const std::string& event) {
  // add a new entry object 
  uint64_t time = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
                  
  EntryPtr entry =
      std::make_unique<Entry>(time, std::map<std::string, std::string>{{"event", event}});
  tracer_->trace(std::move(entry));
}

void Span::finishSpan() {
  // make an entry object with the finish time and send it to the tracer
  uint64_t time = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  // make the record map
  std::map<std::string, std::string> record_map;
  record_map["operation"] = operation_;
  record_map["trace_id"] = span_context_.traceId();
  record_map["span_id"] = span_context_.spanId();
  record_map["start_time"] = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                span_context_.startTime().time_since_epoch())
                                                .count());
  record_map["end_time"] = std::to_string(time);

  // add the tags to the record map
  for (const auto& tag : tags_) {
    record_map[tag.first] = tag.second;
  }

  EntryPtr entry = std::make_unique<Entry>(time, std::move(record_map));
  
  tracer_->trace(std::move(entry));
}

void Span::injectContext(Tracing::TraceContext& trace_context,
                         const Tracing::UpstreamContext& upstream) {
  trace_context.set("TraceId", span_context_.traceId());
  trace_context.set("SpanId", span_context_.spanId());
  trace_context.set("Sampled", span_context_.sampled() ? "1" : "0");
  trace_context.set("StartTime", std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                           span_context_.startTime().time_since_epoch())
                                           .count()));                                     
}

Tracing::SpanPtr Span::spawnChild(const Tracing::Config& config, const std::string& name,
                                  SystemTime start_time) {
  return tracer_->startSpan(config, trace_context_, stream_info_, name, tracing_decision_, start_time, span_context_);
}

void Span::setSampled(bool sampled) {
  span_context_.setSampled(sampled);
}

std::string Span::getBaggage(absl::string_view key) {
  // not implemented
  return EMPTY_STRING;
}

void Span::setBaggage(absl::string_view key, absl::string_view value) {
  // not implemented
}

std::string Span::getTraceId() const { return span_context_.traceId(); }

std::string Span::getSpanId() const { return span_context_.spanId(); }

Tracing::SpanPtr FluentdTracerImpl::startSpan(const Tracing::Config& config,
                                              Tracing::TraceContext& trace_context,
                                              const StreamInfo::StreamInfo& stream_info,
                                              const std::string& operation_name,
                                              Tracing::Decision tracing_decision) {
  // make a new span context
  SpanContext span_context = SpanContext(Hex::uint64ToHex(random_.random()), Hex::uint64ToHex(random_.random()), "", tracing_decision.traced, stream_info.startTime());  

  return std::make_unique<Span>(config, trace_context, stream_info, operation_name, tracing_decision, shared_from_this(), span_context);
}

Tracing::SpanPtr FluentdTracerImpl::startSpan(const Tracing::Config& config,
                                              Tracing::TraceContext& trace_context,
                                              const StreamInfo::StreamInfo& stream_info,
                                              const std::string& operation_name,
                                              Tracing::Decision tracing_decision, 
                                              SystemTime start_time, const SpanContext&previous_span_context) {

  SpanContext span_context = SpanContext(previous_span_context.traceId(), Hex::uint64ToHex(random_.random()), previous_span_context.spanId(), tracing_decision.traced, start_time);

  return std::make_unique<Span>(config, trace_context, stream_info, operation_name, tracing_decision, shared_from_this(), span_context);
}

void FluentdTracerImpl::onEvent(Network::ConnectionEvent event) {
  connecting_ = false;

  if (event == Network::ConnectionEvent::Connected) {
    backoff_strategy_->reset();
    retry_timer_->disableTimer();
    flush();
  } else if (event == Network::ConnectionEvent::LocalClose ||
             event == Network::ConnectionEvent::RemoteClose) {
    ENVOY_LOG(debug, "upstream connection was closed");
    fluentd_stats_.connections_closed_.inc();
    maybeReconnect();
  }
}

void FluentdTracerImpl::trace(EntryPtr&& entry) {
  if (disconnected_ || approximate_message_size_bytes_ >= max_buffer_size_bytes_) {
    fluentd_stats_.entries_lost_.inc();
    // We will lose the data deliberately so the buffer doesn't grow infinitely.
    ENVOY_LOG(info, "Fluentd tracer buffer full, dropping log entry");
    return;
  }

  // stringify the entry
  std::string record_str = "";
  record_str += "time: " + std::to_string(entry->time_) + ", ";
  for (const auto& pair : entry->record_) {
    record_str += pair.first + ": " + pair.second + ", ";
  }
  //ENVOY_LOG(info, "Fluentd tracer entry: {}", record_str);

  //ENVOY_LOG(info, "Fluentd tracer entry time: {}", entry->time_);
  approximate_message_size_bytes_ += sizeof(entry->time_) + entry->record_.size();
  entries_.push_back(std::move(entry));
  //ENVOY_LOG(info, "Fluentd tracer entry added to buffer, buffer count: {}, buffer size: {}", entries_.size(), approximate_message_size_bytes_);

  fluentd_stats_.entries_buffered_.inc();
  if (approximate_message_size_bytes_ >= max_buffer_size_bytes_) {
    // If we exceeded the buffer limit, immediately flush the logs instead of waiting for
    // the next flush interval, to allow new logs to be buffered.
    flush();
  }
}

void FluentdTracerImpl::flush() {
  ASSERT(!disconnected_);
  
  ENVOY_LOG(info, "Fluentd tracer flush, buffer count: {}, buffer size: {}", entries_.size(), approximate_message_size_bytes_);

  if (entries_.empty()) {
    // nothing to send, or we're still waiting for an upstream connection.
    //ENVOY_LOG(info, "Fluentd tracer flush skipped, no entries");
    return;
  }

  if (connecting_) {
    // nothing to send, or we're still waiting for an upstream connection.
    ENVOY_LOG(info, "Fluentd tracer flush skipped, still connecting");
    return;
  }

  if (!client_->connected()) {
    connect();
    ENVOY_LOG(info, "Fluentd tracer flush skipped, not connected");
    return;
  }

  // Creating a Fluentd Forward Protocol Specification (v1) forward mode event as specified in:
  // https://github.com/fluent/fluentd/wiki/Forward-Protocol-Specification-v1#forward-mode

  // *****************************************
  // TODO: Change this to properly pack traces
  // *****************************************

  MessagePackBuffer buffer;
  MessagePackPacker packer(buffer);
  packer.pack_array(3); // 1 - tag field, 2 - entries array, 3 - options map
  packer.pack(tag_);
  packer.pack_array(entries_.size());

  for (auto& entry : entries_) {
    packer.pack_array(2); // 1 - time, 2 - record.
    packer.pack(entry->time_);
    packer.pack_map(entry->record_.size()); // Indicate the number of key-value pairs in the map
    for (const auto& pair : entry->record_) {
      packer.pack(pair.first);  // Pack the key (string)
      packer.pack(pair.second); // Pack the value (string)
    }
  }  
  
  packer.pack(option_);

  if (entries_.size() > 0) {
    ENVOY_LOG(info, "Fluentd tracer flushed {} entries", entries_.size());
  }

  Buffer::OwnedImpl data(buffer.data(), buffer.size());    
  client_->write(data, false);
  fluentd_stats_.events_sent_.inc();
  clearBuffer();
}

void FluentdTracerImpl::connect() {
  connect_attempts_++;
  if (!client_->connect()) {
    ENVOY_LOG(info, "fluentd_tracer: no healthy upstream");
    maybeReconnect();
    return;
  }

  connecting_ = true;
}

void FluentdTracerImpl::maybeReconnect() {
  if (max_connect_attempts_.has_value() && connect_attempts_ >= max_connect_attempts_) {
    ENVOY_LOG(debug, "max connection attempts reached");
    cluster_.info()->trafficStats()->upstream_cx_connect_attempts_exceeded_.inc();
    setDisconnected();
    return;
  }

  uint64_t next_backoff_ms = backoff_strategy_->nextBackOffMs();
  retry_timer_->enableTimer(std::chrono::milliseconds(next_backoff_ms));
  ENVOY_LOG(info, "fluentd_Tracer: reconnect attempt scheduled for {} ms", next_backoff_ms);
}

void FluentdTracerImpl::onBackoffCallback() {
  fluentd_stats_.reconnect_attempts_.inc();
  this->connect();
}

void FluentdTracerImpl::setDisconnected() {
  ENVOY_LOG(info, "fluentd_tracer: set disconnected");
  disconnected_ = true;
  clearBuffer();
  ASSERT(flush_timer_ != nullptr);
  flush_timer_->disableTimer();
}

void FluentdTracerImpl::clearBuffer() {
  ENVOY_LOG(info, "fluentd_tracer: clear buffer");
  entries_.clear();
  approximate_message_size_bytes_ = 0;
}

FluentdTracerCacheImpl::FluentdTracerCacheImpl(Upstream::ClusterManager& cluster_manager,
                                               Stats::Scope& parent_scope,
                                               ThreadLocal::SlotAllocator& tls)
    : cluster_manager_(cluster_manager), stats_scope_(parent_scope.createScope("traces.fluentd")),
      tls_slot_(tls.allocateSlot()) {
  tls_slot_->set(
      [](Event::Dispatcher& dispatcher) { return std::make_shared<ThreadLocalCache>(dispatcher); });
}

FluentdTracerSharedPtr
FluentdTracerCacheImpl::getOrCreateTracer(const FluentdConfigSharedPtr config,
                                          Random::RandomGenerator& random) {
  auto& cache = tls_slot_->getTyped<ThreadLocalCache>();
  const auto cache_key = MessageUtil::hash(*config);
  const auto it = cache.tracers_.find(cache_key);
  if (it != cache.tracers_.end() && !it->second.expired()) {
    return it->second.lock();
  }

  auto* cluster = cluster_manager_.getThreadLocalCluster(config->cluster());
  if (!cluster) {
    return nullptr;
  }

  auto client =
      cluster->tcpAsyncClient(nullptr, std::make_shared<const Tcp::AsyncTcpClientOptions>(false));

  uint64_t base_interval_ms = DefaultBaseBackoffIntervalMs;
  uint64_t max_interval_ms = base_interval_ms * DefaultMaxBackoffIntervalFactor;

  if (config->has_retry_options() && config->retry_options().has_backoff_options()) {
    base_interval_ms = PROTOBUF_GET_MS_OR_DEFAULT(config->retry_options().backoff_options(),
                                                  base_interval, DefaultBaseBackoffIntervalMs);
    max_interval_ms =
        PROTOBUF_GET_MS_OR_DEFAULT(config->retry_options().backoff_options(), max_interval,
                                   base_interval_ms * DefaultMaxBackoffIntervalFactor);
  }

  BackOffStrategyPtr backoff_strategy = std::make_unique<JitteredExponentialBackOffStrategy>(
      base_interval_ms, max_interval_ms, random);

  const auto tracer =
      std::make_shared<FluentdTracerImpl>(*cluster, std::move(client), cache.dispatcher_, *config,
                                          std::move(backoff_strategy), *stats_scope_, random);
  cache.tracers_.emplace(cache_key, tracer);
  return tracer;
}

} // namespace Fluentd
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy
