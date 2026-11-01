cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        SORX_TEST_BUILD_DIR
        SORX_TEST_GENERATOR
        SORX_TEST_CTEST_COMMAND)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing install-consumer input: ${required_variable}")
    endif()
endforeach()

if(NOT DEFINED SORX_TEST_CONFIG OR SORX_TEST_CONFIG STREQUAL "")
    set(SORX_TEST_CONFIG Release)
endif()

set(SORX_TEST_ROOT
    "${SORX_TEST_BUILD_DIR}/release-evidence/install-consumer/${SORX_TEST_CONFIG}")
set(SORX_TEST_PREFIX "${SORX_TEST_ROOT}/prefix")
set(SORX_TEST_SOURCE "${SORX_TEST_ROOT}/source")
set(SORX_TEST_CONSUMER_BUILD "${SORX_TEST_ROOT}/build")

file(REMOVE_RECURSE "${SORX_TEST_ROOT}")
file(MAKE_DIRECTORY "${SORX_TEST_SOURCE}")

set(install_command
    "${CMAKE_COMMAND}"
    --install "${SORX_TEST_BUILD_DIR}"
    --prefix "${SORX_TEST_PREFIX}")
if(SORX_TEST_MULTI_CONFIG)
    list(APPEND install_command --config "${SORX_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "SORX install failed (${install_result})\n"
        "${install_output}\n${install_error}")
endif()

file(WRITE "${SORX_TEST_SOURCE}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(sorx_installed_consumer LANGUAGES CXX)
include(CTest)
find_package(sorx 0.2 CONFIG REQUIRED)
add_executable(sorx_installed_consumer main.cpp)
target_link_libraries(
    sorx_installed_consumer
    PRIVATE
        sorx::runtime
        sorx::ha
        sorx::recorder
        sorx::protocol
        sorx::fixgw_core
        sorx::appliance
        sorx::fastpath
        sorx::risk_fastpath
        sorx::native_connectors
        sorx::native_marketdata
        sorx::recovery
        sorx::audit
        sorx::telemetry
        sorx::dataplane
        sorx::platform
        sorx::marketdata
        sorx::risk
        sorx::adaptive_policy)
if(TARGET sorx::md_core)
    target_link_libraries(sorx_installed_consumer PRIVATE sorx::md_core)
    target_compile_definitions(sorx_installed_consumer PRIVATE SORX_CONSUMER_HAS_MD=1)
endif()
if(TARGET sorx::mockx_md_adapter)
    target_link_libraries(
        sorx_installed_consumer PRIVATE sorx::mockx_md_adapter)
    target_compile_definitions(
        sorx_installed_consumer PRIVATE SORX_CONSUMER_HAS_MOCKX_MD=1)
endif()
if(TARGET sorx::quickfix_adapter)
    target_link_libraries(sorx_installed_consumer PRIVATE sorx::quickfix_adapter)
    target_compile_definitions(
        sorx_installed_consumer PRIVATE SORX_CONSUMER_HAS_QUICKFIX=1)
endif()
if(TARGET sorx::mockx_venue_runtime)
    target_link_libraries(
        sorx_installed_consumer PRIVATE sorx::mockx_venue_runtime)
    target_compile_definitions(
        sorx_installed_consumer PRIVATE SORX_CONSUMER_HAS_MOCKX_VENUE=1)
endif()
add_test(NAME sorx_installed_consumer_runs COMMAND sorx_installed_consumer)
]=])

file(WRITE "${SORX_TEST_SOURCE}/main.cpp" [=[
#include <sorx/persistence/journal.hpp>
#include <sorx/route_rib.hpp>
#include <sorx/runtime.hpp>
#include <sorx/appliance/device_config.hpp>
#include <sorx/audit/collector.hpp>
#include <sorx/connectors/cboe_boe.hpp>
#include <sorx/connectors/cboe_pitch.hpp>
#include <sorx/connectors/nasdaq_itch5.hpp>
#include <sorx/connectors/nasdaq_ouch5.hpp>
#include <sorx/dataplane/route_kernel.hpp>
#include <sorx/fastpath/cancel_pipeline.hpp>
#include <sorx/fastpath/risk_dispatch_pipeline.hpp>
#include <sorx/fixgw/translator.hpp>
#include <sorx/ha/replication.hpp>
#include <sorx/marketdata/fixed_l3_book.hpp>
#include <sorx/platform/readiness.hpp>
#include <sorx/policy/adaptive_policy.hpp>
#include <sorx/protocol/session_auth.hpp>
#include <sorx/recorder/flight_recorder.hpp>
#include <sorx/recovery/fast_recovery.hpp>
#include <sorx/recovery/risk_dispatch_recovery.hpp>
#include <sorx/risk/risk_kernel.hpp>
#include <sorx/telemetry/snapshot.hpp>
#if defined(SORX_CONSUMER_HAS_MD)
#include <sorx/md/feed_engine.hpp>
#endif
#if defined(SORX_CONSUMER_HAS_MOCKX_MD)
#include <sorx/adapters/mockx_md.hpp>
#include <sorx/adapters/mockx_md_session.hpp>
#endif
#if defined(SORX_CONSUMER_HAS_QUICKFIX)
#include <sorx/adapters/quickfix_session.hpp>
#endif
#if defined(SORX_CONSUMER_HAS_MOCKX_VENUE)
#include <sorx/adapters/mockx_venue_runtime.hpp>
#endif

#include <cstdint>
#include <span>
#include <type_traits>

int main() {
    static_assert(sorx::fastpath::kNormalVenueQueueCapacity == 64U);
    static_assert(sorx::policy::kMaximumAdaptiveCandidates == 32U);
    static_assert(sorx::recorder::kMaximumDurableBatchBytes == 4'096U);
    static_assert(sorx::fastpath::kDispatchRecordVersion == 3U);
    static_assert(sorx::fastpath::kRiskDispatchRecordVersion == 3U);
    static_assert(sorx::recovery::kRecoveryCheckpointVersion == 2U);
    static_assert(
        sorx::recovery::kRiskDispatchRecoveryCheckpointVersion == 2U);
    static_assert(
        sorx::recovery::kRiskDispatchRecoveryMinimumCheckpointVersion == 1U);
    static_assert(
        static_cast<std::uint8_t>(
            sorx::recorder::AuthorizationEnvelopeVersion::LegacyRangeV0) ==
        0U);
    static_assert(
        static_cast<std::uint8_t>(
            sorx::recorder::AuthorizationEnvelopeVersion::ExactEffectsV1) ==
        1U);
    static_assert(std::is_trivially_copyable_v<
                  sorx::recorder::DurableEffectPermit>);
    static_assert(sizeof(sorx::recorder::DurableEffectPermit) <= 160U);
    static_assert(sizeof(sorx::fastpath::AuthorizedFrame) <= 12U * 64U);
    static_assert(
        sorx::fastpath::DispatchPipelinePolicy{}.required_domain ==
        sorx::recorder::DurabilityDomain::HardwarePersistent);
    static_assert(
        sorx::fastpath::TokenGatePolicy{}.required_domain ==
        sorx::recorder::DurabilityDomain::HardwarePersistent);
    static_assert(
        sorx::recovery::RiskDispatchRecoveryConfiguration{}
                .required_domain ==
        sorx::recorder::DurabilityDomain::HardwarePersistent);
    static_assert(
        sorx::recovery::RiskDispatchRecoveryConfiguration{}
                .checkpoint_trust_policy ==
        sorx::recovery::RiskDispatchCheckpointTrustPolicy::
            RequireExternalProvenance);
    static_assert(
        sorx::audit::StreamMetadata{}.retention_seconds == 0U);
    static_assert(
        !sorx::audit::StreamMetadata{}
             .retention_qualification_verified);
    static_assert(std::is_trivially_copyable_v<
                  sorx::audit::RetentionQualificationEvidence>);
    static_assert(
        sorx::connectors::nasdaq::itch5::kCertificationStatus ==
        sorx::connectors::native_md::CertificationStatus::Uncertified);
    sorx::RouteRib rib;
    const auto crc = sorx::persistence::crc32c(std::span<const std::byte>{});
    const sorx::recorder::DurableBatch batch;
    if (batch.authorization_envelope_version() !=
        sorx::recorder::AuthorizationEnvelopeVersion::ExactEffectsV1) {
        return 5;
    }
    sorx::recorder::DurableToken legacy_token;
    legacy_token.batch_id = 1U;
    legacy_token.leadership_epoch = 1U;
    legacy_token.configuration_generation = 1U;
    legacy_token.dispatch_authorization = {1U, 1U, 1U};
    legacy_token.authorization_envelope_version =
        sorx::recorder::AuthorizationEnvelopeVersion::LegacyRangeV0;
    legacy_token.payload_size = 1U;
    if (!legacy_token || legacy_token.effect_permit(0U).has_value()) {
        return 6;
    }
    if (rib.set_admin(sorx::VenueId{1U}, sorx::AdminState::Down)) {
        return 1;
    }
#if defined(SORX_CONSUMER_HAS_MOCKX_MD)
    if (sorx::adapters::MockXMdSessionState::Stopped ==
        sorx::adapters::MockXMdSessionState::Failed) {
        return 3;
    }
#endif
#if defined(SORX_CONSUMER_HAS_MOCKX_VENUE)
    if (sorx::adapters::MockXVenueRuntimeState::Stopped ==
        sorx::adapters::MockXVenueRuntimeState::Failed) {
        return 4;
    }
#endif
    return crc == 0U ? 0 : 2;
}
]=])

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${SORX_TEST_SOURCE}"
    -B "${SORX_TEST_CONSUMER_BUILD}"
    -G "${SORX_TEST_GENERATOR}")
if(DEFINED SORX_TEST_GENERATOR_PLATFORM AND
   NOT SORX_TEST_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_command -A "${SORX_TEST_GENERATOR_PLATFORM}")
endif()
if(DEFINED SORX_TEST_GENERATOR_TOOLSET AND
   NOT SORX_TEST_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_command -T "${SORX_TEST_GENERATOR_TOOLSET}")
endif()
foreach(tool IN ITEMS CXX_COMPILER RC_COMPILER MT)
    if(DEFINED SORX_TEST_${tool} AND
       NOT SORX_TEST_${tool} STREQUAL "" AND
       NOT SORX_TEST_${tool} MATCHES "-NOTFOUND$")
        list(APPEND configure_command
            "-DCMAKE_${tool}=${SORX_TEST_${tool}}")
    endif()
endforeach()
if(SORX_TEST_MULTI_CONFIG)
    # Only one configuration was installed into this clean prefix. Restricting
    # the generated consumer prevents CMake from requesting non-installed
    # IMPORTED_LOCATION values for other multi-config variants.
    list(APPEND configure_command
        "-DCMAKE_CONFIGURATION_TYPES=${SORX_TEST_CONFIG}")
else()
    list(APPEND configure_command
        "-DCMAKE_BUILD_TYPE=${SORX_TEST_CONFIG}")
endif()

set(consumer_prefix_path "${SORX_TEST_PREFIX}")
if(DEFINED SORX_TEST_DEPENDENCY_PREFIX_PATH AND
   NOT SORX_TEST_DEPENDENCY_PREFIX_PATH STREQUAL "")
    list(APPEND consumer_prefix_path ${SORX_TEST_DEPENDENCY_PREFIX_PATH})
endif()
string(REPLACE ";" "\\;" consumer_prefix_argument "${consumer_prefix_path}")
list(APPEND configure_command
    "-DCMAKE_PREFIX_PATH=${consumer_prefix_argument}")
if(DEFINED SORX_TEST_QUICKFIX_ROOT AND
   NOT SORX_TEST_QUICKFIX_ROOT STREQUAL "")
    list(APPEND configure_command
        "-DQUICKFIX_ROOT=${SORX_TEST_QUICKFIX_ROOT}")
endif()
if(DEFINED SORX_TEST_QUICKFIX_DIR AND
   NOT SORX_TEST_QUICKFIX_DIR STREQUAL "")
    list(APPEND configure_command
        "-Dquickfix_DIR=${SORX_TEST_QUICKFIX_DIR}")
endif()
if(DEFINED SORX_TEST_TOOLCHAIN_FILE AND
   NOT SORX_TEST_TOOLCHAIN_FILE STREQUAL "")
    list(APPEND configure_command
        "-DCMAKE_TOOLCHAIN_FILE=${SORX_TEST_TOOLCHAIN_FILE}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed SORX consumer configure failed (${configure_result})\n"
        "${configure_output}\n${configure_error}")
endif()

set(build_command
    "${CMAKE_COMMAND}"
    --build "${SORX_TEST_CONSUMER_BUILD}"
    --parallel)
if(SORX_TEST_MULTI_CONFIG)
    list(APPEND build_command --config "${SORX_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed SORX consumer build failed (${build_result})\n"
        "${build_output}\n${build_error}")
endif()

set(test_command
    "${SORX_TEST_CTEST_COMMAND}"
    --test-dir "${SORX_TEST_CONSUMER_BUILD}"
    --output-on-failure)
if(SORX_TEST_MULTI_CONFIG)
    list(APPEND test_command -C "${SORX_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${test_command}
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_output
    ERROR_VARIABLE test_error)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR
        "Installed SORX consumer test failed (${test_result})\n"
        "${test_output}\n${test_error}")
endif()

message(STATUS
    "Installed SORX package configured, linked, and ran a clean consumer")
