#include "source/common/protobuf/protobuf.h"
#include "source/extensions/tracers/fluentd/fluentd_tracer_impl.h"
#include "source/extensions/tracers/fluentd/config.h"
#include "source/common/network/utility.h"

#include "source/common/tracing/null_span_impl.h"

#include "envoy/common/time.h"
#include "envoy/config/trace/v3/fluentd.pb.h"
#include "envoy/registry/registry.h"
#include "envoy/extensions/filters/network/tcp_proxy/v3/tcp_proxy.pb.h"
#include "envoy/config/bootstrap/v3/bootstrap.pb.h"

#include "test/mocks/access_log/mocks.h"
#include "test/mocks/server/tracer_factory_context.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/integration/integration.h"
#include "test/integration/utility.h"
#include "test/test_common/test_time.h"
#include "test/test_common/registry.h"
#include "test/test_common/utility.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"
#include "test/test_common/environment.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"
#include "source/common/tracing/trace_context_impl.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "msgpack.hpp"

using testing::Return;
using testing::ReturnRef;
using testing::AssertionResult;
using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace Fluentd {

class FluentdTracerImplTest : public testing::Test {
public:
  FluentdTracerImplTest() : 
        async_client_(new Tcp::AsyncClient::MockAsyncTcpClient()),
        backoff_strategy_(new MockBackOffStrategy()),
        flush_timer_(new Event::MockTimer(&dispatcher_)),
        retry_timer_(new Event::MockTimer(&dispatcher_)) {}

  void init(int buffer_size_bytes = 1, absl::optional<int> max_connect_attempts = absl::nullopt) {
    EXPECT_CALL(*async_client_, setAsyncTcpClientCallbacks(_));
    EXPECT_CALL(*flush_timer_, enableTimer(_, _));

    config_.set_tag(tag_);

    if (max_connect_attempts.has_value()) {
      config_.mutable_retry_options()->mutable_max_connect_attempts()->set_value(
          max_connect_attempts.value());
    }

    config_.mutable_buffer_size_bytes()->set_value(buffer_size_bytes);
    tracer_ = std::make_unique<FluentdTracerImpl>(
        cluster_, Tcp::AsyncTcpClientPtr{async_client_}, dispatcher_, config_,
        BackOffStrategyPtr{backoff_strategy_}, *stats_store_.rootScope(), random_);
  }

    std::string getExpectedMsgpackPayload(int entries_count) {
        msgpack::sbuffer buffer;
        msgpack::packer<msgpack::sbuffer> packer(buffer);
        packer.pack_array(3);
        packer.pack(tag_);
        packer.pack_array(entries_count);
        for (int idx = 0; idx < entries_count; idx++) {
            packer.pack_array(2);
            packer.pack(time_);
            packer.pack_map(data_.size());
            for (const auto& pair : data_) {
                packer.pack(pair.first);
                packer.pack(pair.second);
            }
        }

        std::map<std::string, std::string> option_ = {{"fluent_signal", "2"}, {"TimeFormat", "DateTime"}};
        packer.pack(option_);
        return std::string(buffer.data(), buffer.size());
    }

  std::string tag_ = "test.tag";
  uint64_t time_ = 123;
  std::map<std::string, std::string> data_ = {{"event", "test"}};
  NiceMock<Upstream::MockThreadLocalCluster> cluster_;
  Tcp::AsyncClient::MockAsyncTcpClient* async_client_;
  MockBackOffStrategy* backoff_strategy_;
  Stats::IsolatedStoreImpl stats_store_;
  Event::MockDispatcher dispatcher_;
  Event::MockTimer* flush_timer_;
  Event::MockTimer* retry_timer_;
  std::unique_ptr<FluentdTracerImpl> tracer_;
  envoy::config::trace::v3::FluentdConfig config_;
  NiceMock<Random::MockRandomGenerator> random_;
};;

TEST_F(FluentdTracerImplTest, NoWriteOnTraceIfNotConnectedToUpstream) {
  init();
  EXPECT_CALL(*async_client_, connect()).WillOnce(Return(true));
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false));
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
}

TEST_F(FluentdTracerImplTest, NoWriteOnTraceIfBufferLimitNotPassed) {
  init(100);
  EXPECT_CALL(*async_client_, connect()).Times(0);
  EXPECT_CALL(*async_client_, connected()).Times(0);
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
}

TEST_F(FluentdTracerImplTest, NoWriteOnTraceIfDisconnectedByRemote) {
  init(1, 1);
  EXPECT_CALL(*flush_timer_, disableTimer());
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false));
  EXPECT_CALL(*async_client_, connect()).WillOnce(Invoke([this]() -> bool {
    tracer_->onEvent(Network::ConnectionEvent::RemoteClose);
    return true;
  }));

  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
}

TEST_F(FluentdTracerImplTest, NoWriteOnTraceIfDisconnectedByLocal) {
  init(1, 1);
  EXPECT_CALL(*flush_timer_, disableTimer());
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false));
  EXPECT_CALL(*async_client_, connect()).WillOnce(Invoke([this]() -> bool {
    tracer_->onEvent(Network::ConnectionEvent::LocalClose);
    return true;
  }));

  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
}

// TODO: test traces more vigorously 
TEST_F(FluentdTracerImplTest, TraceSingleEntry) {
  init(); // Default buffer limit is 0 so single entry should be flushed immediately.
  EXPECT_CALL(*backoff_strategy_, reset());
  EXPECT_CALL(*retry_timer_, disableTimer());
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false)).WillOnce(Return(true));
  EXPECT_CALL(*async_client_, connect()).WillOnce(Invoke([this]() -> bool {
    tracer_->onEvent(Network::ConnectionEvent::Connected);
    return true;
  }));
  EXPECT_CALL(*async_client_, write(_, _))
      .WillOnce(Invoke([&](Buffer::Instance& buffer, bool end_stream) {
        EXPECT_FALSE(end_stream);
        std::string expected_payload = getExpectedMsgpackPayload(1);
        EXPECT_EQ(expected_payload, buffer.toString());
      }));

  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
}

// TODO: figure out the bytes calculation 
TEST_F(FluentdTracerImplTest, TraceTwoEntries) {
  init(12); // First entry is 10 bytes, so first entry should not cause the tracer to flush.

  // First log should not be flushed.
  EXPECT_CALL(*backoff_strategy_, reset());
  EXPECT_CALL(*retry_timer_, disableTimer());
  EXPECT_CALL(*async_client_, connected()).Times(0);
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));

  // Expect second entry to cause all entries to flush.
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false)).WillOnce(Return(true));
  EXPECT_CALL(*async_client_, connect()).WillOnce(Invoke([this]() -> bool {
    tracer_->onEvent(Network::ConnectionEvent::Connected);
    return true;
  }));
  EXPECT_CALL(*async_client_, write(_, _))
      .WillOnce(Invoke([&](Buffer::Instance& buffer, bool end_stream) {
        EXPECT_FALSE(end_stream);
        std::string expected_payload = getExpectedMsgpackPayload(2);
        EXPECT_EQ(expected_payload, buffer.toString());
      }));
  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
}

TEST_F(FluentdTracerImplTest, CallbacksTest) {
  init();
  EXPECT_CALL(*async_client_, connect()).WillOnce(Return(true));
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false));
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
  EXPECT_NO_THROW(tracer_->onAboveWriteBufferHighWatermark());
  EXPECT_NO_THROW(tracer_->onBelowWriteBufferLowWatermark());
  Buffer::OwnedImpl buffer;
  EXPECT_NO_THROW(tracer_->onData(buffer, false));
}

TEST_F(FluentdTracerImplTest, SuccessfulReconnect) {
  init(1, 2);
  EXPECT_CALL(*backoff_strategy_, nextBackOffMs()).WillOnce(Return(1));
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false)).WillOnce(Return(true));
  EXPECT_CALL(*async_client_, connect())
      .WillOnce(Invoke([this]() -> bool {
        EXPECT_CALL(*backoff_strategy_, reset()).Times(0);
        EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(1), _));
        EXPECT_CALL(*retry_timer_, disableTimer()).Times(0);
        tracer_->onEvent(Network::ConnectionEvent::LocalClose);
        return true;
      }))
      .WillOnce(Invoke([this]() -> bool {
        EXPECT_CALL(*backoff_strategy_, reset());
        EXPECT_CALL(*retry_timer_, enableTimer(_, _)).Times(0);
        EXPECT_CALL(*retry_timer_, disableTimer());
        tracer_->onEvent(Network::ConnectionEvent::Connected);
        return true;
      }));
  EXPECT_CALL(*async_client_, write(_, _))
      .WillOnce(Invoke([&](Buffer::Instance& buffer, bool end_stream) {
        EXPECT_FALSE(end_stream);
        std::string expected_payload = getExpectedMsgpackPayload(1);
        EXPECT_EQ(expected_payload, buffer.toString());
      }));

  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
  retry_timer_->invokeCallback();
}

TEST_F(FluentdTracerImplTest, ReconnectFailure) {
  init(1, 2);

  EXPECT_CALL(*backoff_strategy_, nextBackOffMs()).WillOnce(Return(1));
  EXPECT_CALL(*backoff_strategy_, reset()).Times(0);
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(1), _));
  EXPECT_CALL(*retry_timer_, disableTimer()).Times(0);

  EXPECT_CALL(*flush_timer_, disableTimer());
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false));
  EXPECT_CALL(*async_client_, connect())
      .WillOnce(Invoke([this]() -> bool {
        tracer_->onEvent(Network::ConnectionEvent::LocalClose);
        return true;
      }))
      .WillOnce(Invoke([this]() -> bool {
        tracer_->onEvent(Network::ConnectionEvent::LocalClose);
        return true;
      }));

  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
  retry_timer_->invokeCallback();
}

TEST_F(FluentdTracerImplTest, TwoReconnects) {
  init(1, 3);

  EXPECT_CALL(*backoff_strategy_, nextBackOffMs()).WillOnce(Return(1)).WillOnce(Return(1));
  EXPECT_CALL(*backoff_strategy_, reset()).Times(0);
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(1), _)).Times(2);
  EXPECT_CALL(*retry_timer_, disableTimer()).Times(0);

  EXPECT_CALL(*flush_timer_, disableTimer());
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false));
  EXPECT_CALL(*async_client_, connect())
      .WillOnce(Invoke([this]() -> bool {
        tracer_->onEvent(Network::ConnectionEvent::LocalClose);
        return true;
      }))
      .WillOnce(Invoke([this]() -> bool {
        tracer_->onEvent(Network::ConnectionEvent::LocalClose);
        return true;
      }))
      .WillOnce(Invoke([this]() -> bool {
        tracer_->onEvent(Network::ConnectionEvent::LocalClose);
        return true;
      }));

  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
  retry_timer_->invokeCallback();
  retry_timer_->invokeCallback();
}

TEST_F(FluentdTracerImplTest, RetryOnNoHealthyUpstream) {
  init();

  EXPECT_CALL(*backoff_strategy_, nextBackOffMs()).WillOnce(Return(1));
  EXPECT_CALL(*backoff_strategy_, reset()).Times(0);
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(1), _));
  EXPECT_CALL(*retry_timer_, disableTimer()).Times(0);

  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  EXPECT_CALL(*async_client_, connected()).WillOnce(Return(false));
  EXPECT_CALL(*async_client_, connect()).WillOnce(Return(false));
  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
}

TEST_F(FluentdTracerImplTest, NoWriteOnBufferFull) {
  // Setting the buffer to 0 so new log will be thrown.
  init(0);
  EXPECT_CALL(*async_client_, write(_, _)).Times(0);
  EXPECT_CALL(*async_client_, connect()).Times(0);
  EXPECT_CALL(*async_client_, connected()).Times(0);
  tracer_->trace(std::make_unique<Entry>(time_, std::map<std::string, std::string>{{"event", "test"}}));
}




class EnvVarGuard {
public:
  EnvVarGuard(const std::string& name, const std::string& value) : name_(name) {
    if (const char* const previous = std::getenv(name.c_str())) {
      previous_value_ = previous;
    }
    const int overwrite = 1; // Yes, overwrite it.
    TestEnvironment::setEnvVar(name, value, overwrite);
  }

  ~EnvVarGuard() {
    if (previous_value_) {
      const int overwrite = 1; // Yes, overwrite it.
      TestEnvironment::setEnvVar(name_, *previous_value_, overwrite);
    } else {
      TestEnvironment::unsetEnvVar(name_);
    }
  }

private:
  std::string name_;
  absl::optional<std::string> previous_value_;
};

class MockRandomGenerator : public Random::RandomGenerator {
public:
    MOCK_METHOD(uint64_t, random, (), (override));
    MOCK_METHOD(std::string, uuid, (), (override));
};

class FluentdTracerTest : public testing::Test {
public:
  FluentdTracerTest() {
    FluentdTracerFactory factory;
    cache_ = factory.getTracerCacheSingleton(context_.server_factory_context_);
    ASSERT_NE(nullptr, cache_);
  }

  void setup(envoy::config::trace::v3::FluentdConfig& config) {
    auto mock_client_factory = std::make_unique<NiceMock<Grpc::MockAsyncClientFactory>>();
    auto mock_client = std::make_unique<NiceMock<Grpc::MockAsyncClient>>();
    mock_stream_ptr_ = std::make_unique<NiceMock<Grpc::MockAsyncStream>>();
    ON_CALL(*mock_client, startRaw(_, _, _, _)).WillByDefault(Return(mock_stream_ptr_.get()));
    ON_CALL(*mock_client_factory, createUncachedRawAsyncClient())
        .WillByDefault(Return(ByMove(std::move(mock_client))));
    auto& factory_context = context_.server_factory_context_;
    ON_CALL(factory_context, runtime()).WillByDefault(ReturnRef(runtime_));
    ON_CALL(factory_context.cluster_manager_.async_client_manager_, factoryForGrpcService(_, _, _))
        .WillByDefault(Return(ByMove(std::move(mock_client_factory))));
    ON_CALL(factory_context, scope()).WillByDefault(ReturnRef(scope_));

    ThreadLocal::SlotPtr thread_local_slot_ = thread_local_slot_allocator_.allocateSlot();

    auto config_shared_ptr = std::make_shared<envoy::config::trace::v3::FluentdConfig>(config);

    Random::MockRandomGenerator random = Random::MockRandomGenerator();

    // assert config_shared_ptr, random, cache_ are not null
    ASSERT_NE(nullptr, config_shared_ptr);
    ASSERT_NE(nullptr, cache_);
    ASSERT_NE(nullptr, &random);
    

    thread_local_slot_->set(
      [config_shared_ptr = config_shared_ptr, &random, cache_ = cache_](Event::Dispatcher&) {
        return std::make_shared<Driver::ThreadLocalTracer>(
            cache_->getOrCreateTracer(config_shared_ptr, random));
      });
  }

  void setupValidDriver() {
    const std::string yaml_string = R"EOF(
    cluster: "fake_cluster"
    tag: "fake_tag"
    stat_prefix: "envoy.tracers.fluentd"
    )EOF";
    envoy::config::trace::v3::FluentdConfig config;
    TestUtility::loadFromYaml(yaml_string, config);
    setup(config);
  }

protected:
  const std::string operation_name_{"test"};
  NiceMock<Envoy::Server::Configuration::MockTracerFactoryContext> context_;
  NiceMock<Envoy::Server::Configuration::MockServerFactoryContext> server_factory_context_;
  NiceMock<Envoy::Tracing::MockConfig> mock_tracing_config_;
  NiceMock<StreamInfo::MockStreamInfo> stream_info_;
  Event::SimulatedTimeSystem time_system_;
  std::unique_ptr<NiceMock<Grpc::MockAsyncStream>> mock_stream_ptr_{nullptr};
  envoy::config::trace::v3::FluentdConfig config_;
  Tracing::DriverSharedPtr driver_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Event::MockTimer>* timer_;
  NiceMock<Stats::MockIsolatedStatsStore> stats_;
  Stats::Scope& scope_{*stats_.rootScope()};
  FluentdTracerCacheSharedPtr cache_;
  NiceMock<ThreadLocal::MockInstance> thread_local_slot_allocator_;
};

TEST_F(FluentdTracerTest, InitializeDriverValidConfig) {
  setupValidDriver();
  EXPECT_NE(nullptr, driver_);
}
/*

TEST_F(FluentdTracerTest, NoOpMode) {
  // Verify that when the tracer fails to validate its configuration,
  // `startSpan` subsequently returns `NullSpan` instances.
  envoy::config::trace::v3::FluentdConfig config;
  config.set_service("envoy");
  config.set_report_traces(false);
  config.set_report_telemetry(false);
  envoy::config::trace::v3::TraceSamplerConfig::Rule invalid_rule;
  // The `sample_rate`, below, is invalid (should be between 0.0 and 1.0).
  // As a result, the constructor of `Tracer` will fail to initialize the
  // underlying `datadog::tracing::Tracer`, and instead go into a no-op mode
  // where `startSpan` returns `NullSpan` instances.
  invalid_rule.set_sample_rate(-10);
  config.mutable_trace_sampler()->add_rules()->CopyFrom(invalid_rule);

  // Initialize DriverSharedPtr using the definition in config.cc
  FluentdTracerFactory factory;
  auto driver = factory.createTracerDriverTyped(config, context_);

  Tracing::TestTraceContextImpl context{};
  // Any values will do for the sake of this test.
  Tracing::Decision decision;
  decision.set_reason(Tracing::Reason::Sampling);
  decision.set_traced(true);

  const std::string operation_name = "do.thing";
  const SystemTime start = time_.timeSystem().systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(testing::Return(start));

  const Tracing::SpanPtr span =
      driver->startSpan(Tracing::MockConfig{}, context, stream_info_, operation_name, decision);
  ASSERT_TRUE(span);
  const auto as_null_span = dynamic_cast<Tracing::NullSpan*>(span.get());
  EXPECT_NE(nullptr, as_null_span);
}

TEST_F(FluentdTracerTest, SpanProperties) {
  // Verify that span-affecting parameters to `startSpan` are reflected in the
  // resulting span.
  envoy::config::trace::v3::FluentdConfig config;
  config.set_service("envoy");
  config.set_report_traces(false);
  config.set_report_telemetry(false);
  // Configure the tracer to keep all spans. We then override that
  // configuration in the `Tracing::Decision`, below.
  config.mutable_trace_sampler()->set_sample_rate(1.0); // 100%

  Tracer tracer("fake_cluster", "test_host", config, cluster_manager_, *store_.rootScope(),
                thread_local_slot_allocator_, time_);

  Tracing::TestTraceContextImpl context{};
  // Any values will do for the sake of this test.
  Tracing::Decision decision;
  decision.set_reason(Tracing::Reason::Sampling);
  decision.set_traced(true);

  const std::string operation_name = "do.thing";
  const SystemTime start = time_.timeSystem().systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(testing::Return(start));

  const Tracing::SpanPtr span =
      tracer.startSpan(Tracing::MockConfig{}, context, stream_info_, operation_name, decision);
  ASSERT_TRUE(span);
  const auto as_dd_span_wrapper = dynamic_cast<Span*>(span.get());
  EXPECT_NE(nullptr, as_dd_span_wrapper);

  const datadog::tracing::Optional<datadog::tracing::Span>& maybe_dd_span =
      as_dd_span_wrapper->impl();
  ASSERT_TRUE(maybe_dd_span);
  const datadog::tracing::Span& dd_span = *maybe_dd_span;

  // Verify that the span has the expected service name, operation name,
  // resource name, start time, and sampling decision.
  // Note that the `operation_name` we specified above becomes the
  // `resource_name()` of the resulting Datadog span, while the Datadog span's
  // `name()` (operation name) is hard-coded to "envoy.proxy."
  EXPECT_EQ("envoy.proxy", dd_span.name());
  EXPECT_EQ("do.thing", dd_span.resource_name());
  EXPECT_EQ("envoy", dd_span.service_name());
  ASSERT_TRUE(dd_span.trace_segment().sampling_decision());
  EXPECT_EQ(int(datadog::tracing::SamplingPriority::USER_DROP),
            dd_span.trace_segment().sampling_decision()->priority);
  EXPECT_EQ(start, dd_span.start_time().wall);
}

TEST_F(FluentdTracerTest, ExtractionSuccess) {
  // Verify that if there is trace information to extract from the
  // `TraceContext` supplied to `startSpan`, that the resulting span is part of
  // the extracted trace.
  envoy::config::trace::v3::FluentdConfig config;
  config.set_service("envoy");
  config.set_report_traces(false);
  config.set_report_telemetry(false);

  Tracer tracer("fake_cluster", "test_host", config, cluster_manager_, *store_.rootScope(),
                thread_local_slot_allocator_, time_);

  // Any values will do for the sake of this test.
  Tracing::Decision decision;
  decision.set_reason(Tracing::Reason::Sampling);
  decision.set_traced(true);

  const std::string operation_name = "do.thing";
  const SystemTime start = time_.timeSystem().systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(testing::Return(start));

  // trace context in the Datadog style
  Tracing::TestTraceContextImpl context{{"x-datadog-trace-id", "1234"},
                                        {"x-datadog-parent-id", "5678"}};

  const Tracing::SpanPtr span =
      tracer.startSpan(Tracing::MockConfig{}, context, stream_info_, operation_name, decision);
  ASSERT_TRUE(span);
  const auto as_dd_span_wrapper = dynamic_cast<Span*>(span.get());
  EXPECT_NE(nullptr, as_dd_span_wrapper);

  const datadog::tracing::Optional<datadog::tracing::Span>& maybe_dd_span =
      as_dd_span_wrapper->impl();
  ASSERT_TRUE(maybe_dd_span);
  const datadog::tracing::Span& dd_span = *maybe_dd_span;

  EXPECT_EQ(1234, dd_span.trace_id().low);
  ASSERT_TRUE(dd_span.parent_id());
  EXPECT_EQ(5678, *dd_span.parent_id());
}

TEST_F(FluentdTracerTest, ExtractionFailure) {
  // Verify that if there is invalid trace information in the `TraceContext`
  // supplied to `startSpan`, that the resulting span is nonetheless valid (it
  // will be the start of a new trace).
  envoy::config::trace::v3::FluentdConfig config;
  config.set_service("envoy");
  config.set_report_traces(false);
  config.set_report_telemetry(false);

  Tracer tracer("fake_cluster", "test_host", config, cluster_manager_, *store_.rootScope(),
                thread_local_slot_allocator_, time_);

  // Any values will do for the sake of this test.
  Tracing::Decision decision;
  decision.set_reason(Tracing::Reason::Sampling);
  decision.set_traced(true);

  const std::string operation_name = "do.thing";
  const SystemTime start = time_.timeSystem().systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(testing::Return(start));

  // invalid trace context in the Datadog style
  Tracing::TestTraceContextImpl context{{"x-datadog-trace-id", "nope"},
                                        {"x-datadog-parent-id", "nice try"}};

  const Tracing::SpanPtr span =
      tracer.startSpan(Tracing::MockConfig{}, context, stream_info_, operation_name, decision);
  ASSERT_TRUE(span);
  const auto as_dd_span_wrapper = dynamic_cast<Span*>(span.get());
  EXPECT_NE(nullptr, as_dd_span_wrapper);

  const datadog::tracing::Optional<datadog::tracing::Span>& maybe_dd_span =
      as_dd_span_wrapper->impl();
  ASSERT_TRUE(maybe_dd_span);
}
*/

} // namespace Fluentd
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy

