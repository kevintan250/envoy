#include "source/extensions/tracers/fluentd/fluentd_tracer_impl.h"

#include "fluentd_tracer_impl.h"
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

// See https://www.w3.org/TR/trace-context/#traceparent-header
constexpr int kTraceparentHeaderSize = 55; // 2 + 1 + 32 + 1 + 16 + 1 + 2
constexpr int kVersionHexSize = 2;
constexpr int kTraceIdHexSize = 32;
constexpr int kParentIdHexSize = 16;
constexpr int kTraceFlagsHexSize = 2;

bool isValidHex(const absl::string_view& input) {
  return std::all_of(input.begin(), input.end(),
                     [](const char& c) { return absl::ascii_isxdigit(c); });
}

bool isAllZeros(const absl::string_view& input) {
  return std::all_of(input.begin(), input.end(), [](const char& c) { return c == '0'; });
}

SpanContextExtractor::SpanContextExtractor(Tracing::TraceContext& trace_context)
    : trace_context_(trace_context) {}

SpanContextExtractor::~SpanContextExtractor() = default;

bool SpanContextExtractor::propagationHeaderPresent() {
  auto propagation_header = FluentdConstants::get().TRACE_PARENT.get(trace_context_);
  return propagation_header.has_value();
}

absl::StatusOr<SpanContext> SpanContextExtractor::extractSpanContext() {
  auto propagation_header = FluentdConstants::get().TRACE_PARENT.get(trace_context_);
  if (!propagation_header.has_value()) {
    // We should have already caught this, but just in case.
    return absl::InvalidArgumentError("No propagation header found");
  }
  auto header_value_string = propagation_header.value();

  if (header_value_string.size() != kTraceparentHeaderSize) {
    return absl::InvalidArgumentError("Invalid traceparent header length");
  }
  // Try to split it into its component parts:
  std::vector<absl::string_view> propagation_header_components =
      absl::StrSplit(header_value_string, '-', absl::SkipEmpty());
  if (propagation_header_components.size() != 4) {
    return absl::InvalidArgumentError("Invalid traceparent hyphenation");
  }
  absl::string_view version = propagation_header_components[0];
  absl::string_view trace_id = propagation_header_components[1];
  absl::string_view parent_id = propagation_header_components[2];
  absl::string_view trace_flags = propagation_header_components[3];
  if (version.size() != kVersionHexSize || trace_id.size() != kTraceIdHexSize ||
      parent_id.size() != kParentIdHexSize || trace_flags.size() != kTraceFlagsHexSize) {
    return absl::InvalidArgumentError("Invalid traceparent field sizes");
  }
  if (!isValidHex(version) || !isValidHex(trace_id) || !isValidHex(parent_id) ||
      !isValidHex(trace_flags)) {
    return absl::InvalidArgumentError("Invalid header hex");
  }
  // As per the traceparent header definition, if the trace-id or parent-id are all zeros, they are
  // invalid and must be ignored.
  if (isAllZeros(trace_id)) {
    return absl::InvalidArgumentError("Invalid trace id");
  }
  if (isAllZeros(parent_id)) {
    return absl::InvalidArgumentError("Invalid parent id");
  }

  // Set whether or not the span is sampled from the trace flags.
  // See https://w3c.github.io/trace-context/#trace-flags.
  char decoded_trace_flags = absl::HexStringToBytes(trace_flags).front();
  bool sampled = (decoded_trace_flags & 1);

  // If a tracestate header is received without an accompanying traceparent header,
  // it is invalid and MUST be discarded. Because we're already checking for the
  // traceparent header above, we don't need to check here.
  // See https://www.w3.org/TR/trace-context/#processing-model-for-working-with-trace-context
  absl::string_view tracestate_key = FluentdConstants::get().TRACE_STATE.key();
  std::vector<std::string> tracestate_values;
  // Multiple tracestate header fields MUST be handled as specified by RFC7230 Section 3.2.2 Field
  // Order.
  trace_context_.forEach(
      [&tracestate_key, &tracestate_values](absl::string_view key, absl::string_view value) {
        if (key == tracestate_key) {
          tracestate_values.push_back(std::string{value});
        }
        return true;
      });
  std::string tracestate = absl::StrJoin(tracestate_values, ",");

  SpanContext span_context(version, trace_id, parent_id, sampled, tracestate);
  return span_context;
}


constexpr absl::string_view kDefaultVersion = "00";

const Tracing::TraceContextHandler& traceParentHeader() {
  CONSTRUCT_ON_FIRST_USE(Tracing::TraceContextHandler, "traceparent");
}

const Tracing::TraceContextHandler& traceStateHeader() {
  CONSTRUCT_ON_FIRST_USE(Tracing::TraceContextHandler, "tracestate");
}

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

  SpanContextExtractor extractor(trace_context);
  if (!extractor.propagationHeaderPresent()) {
    // No propagation header, so we can create a fresh span with the given decision.

    return tracer.startSpan(trace_context, stream_info.startTime(), operation_name, tracing_decision);
  }
  else {
    // Try to extract the span context. If we can't, just return a null span.
    absl::StatusOr<SpanContext> span_context = extractor.extractSpanContext();
    if (span_context.ok()) {
      
      return tracer.startSpan(trace_context, stream_info.startTime(), operation_name, tracing_decision, span_context.value());
        
    } else {
      ENVOY_LOG(trace, "Unable to extract span context: ", span_context.status());
      return std::make_unique<Tracing::NullSpan>();
    }
  }
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
Span::Span(Tracing::TraceContext& trace_context, SystemTime start_time, const std::string& operation_name,
           Tracing::Decision tracing_decision, FluentdTracerSharedPtr tracer, const SpanContext& span_context)
    : trace_context_(trace_context), start_time_(start_time), operation_(operation_name),
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
  record_map["span_id"] = span_context_.parentId();
  record_map["start_time"] = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                start_time_.time_since_epoch())
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
  
  std::string trace_id_hex = span_context_.traceId();
  std::string parent_id_hex = span_context_.parentId();
  std::vector<uint8_t> trace_flags_vec{sampled()};
  std::string trace_flags_hex = Hex::encode(trace_flags_vec);
  std::string traceparent_header_value = absl::StrCat(kDefaultVersion, "-", trace_id_hex, "-", parent_id_hex, "-", trace_flags_hex);

  // Set the traceparent in the trace_context.
  traceParentHeader().setRefKey(trace_context, traceparent_header_value);
  // Also set the tracestate.
  traceStateHeader().setRefKey(trace_context, span_context_.tracestate());                               
}

Tracing::SpanPtr Span::spawnChild(const Tracing::Config&, const std::string& name,
                                  SystemTime start_time) {

                                    // TODO: check if a new span_context_ needs to be built

  SpanContext span_context = SpanContext(kDefaultVersion, span_context_.traceId(), span_context_.parentId(), sampled(), span_context_.tracestate());
  return tracer_->startSpan(trace_context_, start_time, name, tracing_decision_, span_context);
}

void Span::setSampled(bool sampled) {
  sampled_ = sampled;
}

std::string Span::getBaggage(absl::string_view key) {
  // not implemented
  return EMPTY_STRING;
}

void Span::setBaggage(absl::string_view key, absl::string_view value) {
  // not implemented
}

std::string Span::getTraceId() const { return span_context_.traceId(); }

std::string Span::getSpanId() const { return span_context_.parentId(); }

Tracing::SpanPtr FluentdTracerImpl::startSpan(Tracing::TraceContext& trace_context,
                                              SystemTime start_time,
                                              const std::string& operation_name,
                                              Tracing::Decision tracing_decision) {

  // make a new span context
  uint64_t trace_id_high = random_.random();
  uint64_t trace_id = random_.random();

  uint64_t span_id = random_.random();

  SpanContext span_context = SpanContext(kDefaultVersion, absl::StrCat(Hex::uint64ToHex(trace_id_high), Hex::uint64ToHex(trace_id)), Hex::uint64ToHex(span_id), tracing_decision.traced, "");
 
  Span new_span(trace_context, start_time, operation_name, tracing_decision, shared_from_this(), span_context);

  new_span.setSampled(tracing_decision.traced);

  return std::make_unique<Span>(new_span);
}

Tracing::SpanPtr FluentdTracerImpl::startSpan(Tracing::TraceContext& trace_context,
                                              SystemTime start_time,
                                              const std::string& operation_name,
                                              Tracing::Decision tracing_decision, const SpanContext&previous_span_context) {

  SpanContext span_context = SpanContext(kDefaultVersion, previous_span_context.traceId(), Hex::uint64ToHex(random_.random()), previous_span_context.sampled(), previous_span_context.tracestate());

  Span new_span(trace_context, start_time, operation_name, tracing_decision, shared_from_this(), span_context);

  new_span.setSampled(previous_span_context.sampled());

  return std::make_unique<Span>(new_span);
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
