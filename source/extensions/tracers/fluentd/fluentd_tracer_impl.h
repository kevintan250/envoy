#pragma once

#include <chrono>

#include "envoy/config/trace/v3/fluentd.pb.h"
#include "envoy/config/trace/v3/fluentd.pb.validate.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/tracing/trace_driver.h"

#include "source/common/common/logger.h"
#include "source/extensions/tracers/common/factory_base.h"

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace Fluentd {

using FluentdConfig = envoy::config::trace::v3::FluentdConfig;
using FluentdConfigSharedPtr = std::shared_ptr<FluentdConfig>;

static constexpr uint64_t DefaultBaseBackoffIntervalMs = 500;
static constexpr uint64_t DefaultMaxBackoffIntervalFactor = 10;
static constexpr uint64_t DefaultBufferFlushIntervalMs = 1000;
static constexpr uint64_t DefaultMaxBufferSize = 16384;

// Entry represents a single Fluentd message, msgpack format based, as specified in:
// https://github.com/fluent/fluentd/wiki/Forward-Protocol-Specification-v1#entry
class Entry {
public:
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;
  Entry(uint64_t time, std::map<std::string, std::string>&& record)
      : time_(time), record_(std::move(record)) {}

  const uint64_t time_;
  const std::map<std::string, std::string> record_;
};

using EntryPtr = std::unique_ptr<Entry>;

class FluentdTracer {
public:
  virtual ~FluentdTracer() = default;

  /**
   * Send the Fluentd formatted message over the upstream TCP connection.
   */
  virtual void trace(EntryPtr&& entry) PURE;
};

using FluentdTracerWeakPtr = std::weak_ptr<FluentdTracer>;
using FluentdTracerSharedPtr = std::shared_ptr<FluentdTracer>;

#define TRACER_FLUENTD_STATS(COUNTER, GAUGE)                                                       \
  COUNTER(entries_lost)                                                                            \
  COUNTER(entries_buffered)                                                                        \
  COUNTER(events_sent)                                                                             \
  COUNTER(reconnect_attempts)                                                                      \
  COUNTER(connections_closed)

struct TracerFluentdStats {
  TRACER_FLUENTD_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

class FluentdTracerImpl : public Tcp::AsyncTcpClientCallbacks,
                          public Tracing::Driver,
                          public FluentdTracer,
                          public Logger::Loggable<Logger::Id::tracing> {
public:
  FluentdTracerImpl(Upstream::ThreadLocalCluster& cluster, Tcp::AsyncTcpClientPtr client,
                    Event::Dispatcher& dispatcher,
                    const FluentdConfig& config,
                    BackOffStrategyPtr backoff_strategy, Stats::Scope& parent_scope);

  // Tcp::AsyncTcpClientCallbacks
  void onEvent(Network::ConnectionEvent event) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}
  void onData(Buffer::Instance&, bool) override {}

  // Tracing::Driver
  Tracing::SpanPtr startSpan(const Tracing::Config& config, Tracing::TraceContext& trace_context,
                             const StreamInfo::StreamInfo& stream_info,
                             const std::string& operation_name,
                             Tracing::Decision tracing_decision) override;

  // FluentdTracer
  void trace(EntryPtr&& entry) override;

private:
  void flush();
  void connect();
  void maybeReconnect();
  void onBackoffCallback();
  void setDisconnected();
  void clearBuffer();
  
  bool disconnected_ = false;
  bool connecting_ = false;
  std::string tag_;
  std::string id_;
  uint32_t connect_attempts_{0};
  absl::optional<uint32_t> max_connect_attempts_{};
  const Stats::ScopeSharedPtr stats_scope_;
  TracerFluentdStats fluentd_stats_;
  std::vector<EntryPtr> entries_;
  uint64_t approximate_message_size_bytes_ = 0;
  Upstream::ThreadLocalCluster& cluster_;
  const BackOffStrategyPtr backoff_strategy_;
  const Tcp::AsyncTcpClientPtr client_;
  const std::chrono::milliseconds buffer_flush_interval_msec_;
  const uint64_t max_buffer_size_bytes_;
  const Event::TimerPtr retry_timer_;
  const Event::TimerPtr flush_timer_;
  std::map<std::string, std::string> option_;
};

class FluentdTracerCache {
public:
  virtual ~FluentdTracerCache() = default;

  /**
   * Get existing tracer or create a new one for the given configuration.
   * @return FluentdTracerSharedPtr ready for logging requests.
   */
  virtual FluentdTracerSharedPtr getOrCreateTracer(const FluentdConfigSharedPtr config,
                                                   Random::RandomGenerator& random) PURE;
};

using FluentdTracerCacheSharedPtr = std::shared_ptr<FluentdTracerCache>;

class FluentdTracerCacheImpl : public Singleton::Instance, public FluentdTracerCache {
public:
  FluentdTracerCacheImpl(Upstream::ClusterManager& cluster_manager, Stats::Scope& parent_scope,
                         ThreadLocal::SlotAllocator& tls);

  // FluentdTracerCache
  FluentdTracerSharedPtr getOrCreateTracer(const FluentdConfigSharedPtr config,
                                           Random::RandomGenerator& random) override;

private:
  /**
   * Per-thread cache.
   */
  struct ThreadLocalCache : public ThreadLocal::ThreadLocalObject {
    ThreadLocalCache(Event::Dispatcher& dispatcher) : dispatcher_(dispatcher) {}

    Event::Dispatcher& dispatcher_;
    // Tracers indexed by the hash of tracer's configuration.
    absl::flat_hash_map<std::size_t, FluentdTracerWeakPtr> tracers_;
  };

  Upstream::ClusterManager& cluster_manager_;
  Stats::ScopeSharedPtr stats_scope_;
  ThreadLocal::SlotPtr tls_slot_;
};

using TracerPtr = std::unique_ptr<FluentdTracerImpl>;

class Driver : Logger::Loggable<Logger::Id::tracing>, public Tracing::Driver {
public:
  Driver(const FluentdConfigSharedPtr fluentd_config,
         Server::Configuration::TracerFactoryContext& context, 
         FluentdTracerCacheSharedPtr tracer_cache);

  // Tracing::Driver
  Tracing::SpanPtr startSpan(const Tracing::Config& config, Tracing::TraceContext& trace_context,
                             const StreamInfo::StreamInfo& stream_info,
                             const std::string& operation_name,
                             Tracing::Decision tracing_decision) override;

private:
  class ThreadLocalTracer : public ThreadLocal::ThreadLocalObject {
  public:
    ThreadLocalTracer(FluentdTracerSharedPtr tracer)
        : tracer_(std::move(tracer)) {}

  private:
    FluentdTracerSharedPtr tracer_;
  };

  ThreadLocal::SlotPtr tls_slot_;
  const FluentdConfigSharedPtr fluentd_config_;
  FluentdTracerCacheSharedPtr tracer_cache_;
};

class Span : public Tracing::Span {
public:
  Span(const Tracing::Config& config, Tracing::TraceContext& trace_context,
       const StreamInfo::StreamInfo& stream_info, const std::string& operation_name,
       Tracing::Decision tracing_decision); // TODO: figure out what this initializer does?????

  // Tracing::Span
  void setOperation(absl::string_view operation) override;
  void setTag(absl::string_view name, absl::string_view value) override;
  void log(SystemTime timestamp, const std::string& event) override;
  void finishSpan() override;
  void injectContext(Tracing::TraceContext& trace_context,
                     const Tracing::UpstreamContext& upstream) override;
  Tracing::SpanPtr spawnChild(const Tracing::Config& config, const std::string& name,
                              SystemTime start_time) override;
  void setSampled(bool sampled) override;
  std::string getBaggage(absl::string_view key) override;
  void setBaggage(absl::string_view key, absl::string_view value) override;
  std::string getTraceId() const override;
  std::string getSpanId() const override;

private:
  // config
  Tracing::TraceContext& trace_context_;
  const StreamInfo::StreamInfo& stream_info_;
  FluentdTracerSharedPtr tracer_;
  std::string operation_;
  Tracing::Decision tracing_decision_;
  const std::string trace_id_;
  const std::string span_id_;
  const std::chrono::steady_clock::time_point start_time_;
  std::map<std::string, std::string> tags_;
};

} // namespace Fluentd
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy
