load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)
load(
    "//test/extensions:extensions_build_system.bzl",
    "envoy_extension_cc_test",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_extension_cc_test(
    name = "tracer_test",
    srcs = ["tracer_test.cc"],
    extension_names = ["envoy.tracers.fluentd"],
    rbe_pool = "2core",
    # TODO(wrowe): envoy_extension_ rules don't currently exclude windows extensions
    tags = ["skip_on_windows"],
    deps = [
        "//source/common/config:utility_lib",
        "//source/common/tracing:http_tracer_lib",
        "//source/common/tracing:trace_context_lib",
        "//source/extensions/tracers/common:factory_base_lib",
        "//source/extensions/tracers/fluentd:config",
        "//source/extensions/tracers/fluentd:fluentd_tracer_lib",
        "//source/extensions/filters/network/tcp_proxy:config",
        
        "//test/mocks/server:tracer_factory_context_mocks",
        "//test/mocks/server:tracer_factory_mocks",
        "//test/integration:integration_lib",
        "//test/test_common:registry_lib",
        "//test/test_common:utility_lib",
        "//test/mocks/http:http_mocks",
        "//test/mocks/server:server_factory_context_mocks",
        "//test/mocks/stream_info:stream_info_mocks",
        "//test/mocks/tracing:tracing_mocks",
        "@envoy_api//envoy/config/trace/v3:pkg_cc_proto",
        "@envoy_api//envoy/config/bootstrap/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/filters/network/tcp_proxy/v3:pkg_cc_proto",
        
        "@com_github_msgpack_cpp//:msgpack",
    ],
)

envoy_extension_cc_test(
    name = "config_test",
    srcs = ["config_test.cc"],
    extension_names = ["envoy.tracers.fluentd"],
    rbe_pool = "2core",
    # TODO(wrowe): envoy_extension_ rules don't currently exclude windows extensions
    tags = ["skip_on_windows"],
    deps = [
        "//source/extensions/tracers/fluentd:config",
        "//test/mocks/server:tracer_factory_context_mocks",
        "//test/mocks/server:tracer_factory_mocks",
        "//test/test_common:utility_lib",
        "@envoy_api//envoy/config/trace/v3:pkg_cc_proto",
    ],
)
