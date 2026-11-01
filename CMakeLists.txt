cmake_minimum_required(VERSION 3.20)

project(sorx VERSION 0.2.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(SORX_ENABLE_QUICKFIX "Build the QuickFIX session adapter" OFF)
option(SORX_ENABLE_INTEGRATION "Enable multi-MockX integration tests" OFF)
option(SORX_BUILD_APPLIANCE "Build the SORX appliance process binaries" ON)
set(SORX_MOCKX_ROOT "" CACHE PATH "Path to the sibling MockX repository")

function(sorx_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(
        ${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE /W4 /WX /permissive- /EHsc /Zc:__cplusplus)
        target_compile_definitions(
            ${target}
            PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    else()
        target_compile_options(
            ${target}
            PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# Optimized CMake configurations define NDEBUG by default. Every compiled
# CTest executable explicitly undefines it so assert-based acceptance checks
# retain identical semantics in Debug, Release, and RelWithDebInfo.
function(sorx_enable_test_assertions target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endfunction()

add_library(
    sorx_core
    STATIC
        src/route_rib.cpp
        src/routing_policy.cpp
        src/order_core.cpp
        src/transcript.cpp)
add_library(sorx::core ALIAS sorx_core)
set_target_properties(sorx_core PROPERTIES EXPORT_NAME core)
sorx_target_defaults(sorx_core)

add_library(sorx_config STATIC src/config.cpp)
add_library(sorx::config ALIAS sorx_config)
set_target_properties(sorx_config PROPERTIES EXPORT_NAME config)
sorx_target_defaults(sorx_config)
target_link_libraries(sorx_config PUBLIC sorx_core)

add_library(
    sorx_persistence
    STATIC
        src/persistence/journal.cpp)
add_library(sorx::persistence ALIAS sorx_persistence)
set_target_properties(sorx_persistence PROPERTIES EXPORT_NAME persistence)
sorx_target_defaults(sorx_persistence)

add_library(sorx_ha STATIC src/ha/replication.cpp)
add_library(sorx::ha ALIAS sorx_ha)
set_target_properties(sorx_ha PROPERTIES EXPORT_NAME ha)
sorx_target_defaults(sorx_ha)

add_library(sorx_recorder STATIC src/recorder/flight_recorder.cpp)
add_library(sorx::recorder ALIAS sorx_recorder)
set_target_properties(sorx_recorder PROPERTIES EXPORT_NAME recorder)
sorx_target_defaults(sorx_recorder)

add_library(
    sorx_protocol
    STATIC
        src/protocol/sobp_v1.cpp
        src/protocol/session_auth.cpp)
add_library(sorx::protocol ALIAS sorx_protocol)
set_target_properties(sorx_protocol PROPERTIES EXPORT_NAME protocol)
sorx_target_defaults(sorx_protocol)

add_library(sorx_fixgw_core STATIC src/fixgw/translator.cpp)
add_library(sorx::fixgw_core ALIAS sorx_fixgw_core)
set_target_properties(sorx_fixgw_core PROPERTIES EXPORT_NAME fixgw_core)
sorx_target_defaults(sorx_fixgw_core)
target_link_libraries(sorx_fixgw_core PUBLIC sorx_protocol)

add_library(
    sorx_appliance
    STATIC
        src/appliance/device_config.cpp
        src/appliance/app_main.cpp)
add_library(sorx::appliance ALIAS sorx_appliance)
set_target_properties(sorx_appliance PROPERTIES EXPORT_NAME appliance)
sorx_target_defaults(sorx_appliance)

add_library(
    sorx_fastpath
    STATIC
        src/fastpath/contracts.cpp
        src/fastpath/dispatch_pipeline.cpp
        src/fastpath/cancel_pipeline.cpp)
add_library(sorx::fastpath ALIAS sorx_fastpath)
set_target_properties(sorx_fastpath PROPERTIES EXPORT_NAME fastpath)
sorx_target_defaults(sorx_fastpath)
target_link_libraries(
    sorx_fastpath
    PUBLIC
        sorx_recorder
        sorx_dataplane
        sorx_ha)

add_library(
    sorx_risk_fastpath
    STATIC src/fastpath/risk_dispatch_pipeline.cpp)
add_library(sorx::risk_fastpath ALIAS sorx_risk_fastpath)
set_target_properties(
    sorx_risk_fastpath
    PROPERTIES EXPORT_NAME risk_fastpath)
sorx_target_defaults(sorx_risk_fastpath)
target_link_libraries(
    sorx_risk_fastpath
    PUBLIC
        sorx_fastpath
        sorx_risk)

add_library(
    sorx_native_connectors
    STATIC
        src/connectors/nasdaq_ouch5.cpp
        src/connectors/cboe_boe.cpp)
add_library(sorx::native_connectors ALIAS sorx_native_connectors)
set_target_properties(
    sorx_native_connectors
    PROPERTIES EXPORT_NAME native_connectors)
sorx_target_defaults(sorx_native_connectors)
target_link_libraries(sorx_native_connectors PUBLIC sorx_fastpath)

add_library(
    sorx_native_marketdata
    STATIC
        src/connectors/nasdaq_itch5.cpp
        src/connectors/cboe_pitch.cpp)
add_library(sorx::native_marketdata ALIAS sorx_native_marketdata)
set_target_properties(
    sorx_native_marketdata
    PROPERTIES EXPORT_NAME native_marketdata)
sorx_target_defaults(sorx_native_marketdata)
target_link_libraries(sorx_native_marketdata PUBLIC sorx_marketdata)

add_library(
    sorx_recovery
    STATIC
        src/recovery/fast_recovery.cpp
        src/recovery/risk_dispatch_recovery.cpp)
add_library(sorx::recovery ALIAS sorx_recovery)
set_target_properties(sorx_recovery PROPERTIES EXPORT_NAME recovery)
sorx_target_defaults(sorx_recovery)
target_link_libraries(
    sorx_recovery
    PUBLIC
        sorx_dataplane
        sorx_recorder
        sorx_risk_fastpath
    PRIVATE sorx_fastpath)

add_library(sorx_audit STATIC src/audit/collector.cpp)
add_library(sorx::audit ALIAS sorx_audit)
set_target_properties(sorx_audit PROPERTIES EXPORT_NAME audit)
sorx_target_defaults(sorx_audit)
target_link_libraries(sorx_audit PUBLIC sorx_recorder)

add_library(sorx_telemetry INTERFACE)
add_library(sorx::telemetry ALIAS sorx_telemetry)
set_target_properties(sorx_telemetry PROPERTIES EXPORT_NAME telemetry)
target_compile_features(sorx_telemetry INTERFACE cxx_std_20)
target_include_directories(
    sorx_telemetry
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(sorx_dataplane STATIC src/dataplane/route_kernel.cpp)
add_library(sorx::dataplane ALIAS sorx_dataplane)
set_target_properties(sorx_dataplane PROPERTIES EXPORT_NAME dataplane)
sorx_target_defaults(sorx_dataplane)
target_link_libraries(sorx_dataplane PUBLIC sorx_core)

add_library(sorx_platform STATIC src/platform/readiness.cpp)
add_library(sorx::platform ALIAS sorx_platform)
set_target_properties(sorx_platform PROPERTIES EXPORT_NAME platform)
sorx_target_defaults(sorx_platform)

add_library(
    sorx_marketdata
    STATIC
        src/marketdata/fixed_l3_book.cpp
        src/marketdata/feed_arbiter.cpp)
add_library(sorx::marketdata ALIAS sorx_marketdata)
set_target_properties(sorx_marketdata PROPERTIES EXPORT_NAME marketdata)
sorx_target_defaults(sorx_marketdata)

add_library(sorx_risk STATIC src/risk/risk_kernel.cpp)
add_library(sorx::risk ALIAS sorx_risk)
set_target_properties(sorx_risk PROPERTIES EXPORT_NAME risk)
sorx_target_defaults(sorx_risk)
target_link_libraries(sorx_risk PUBLIC sorx_core)

add_library(sorx_adaptive_policy STATIC src/policy/adaptive_policy.cpp)
add_library(sorx::adaptive_policy ALIAS sorx_adaptive_policy)
set_target_properties(
    sorx_adaptive_policy
    PROPERTIES EXPORT_NAME adaptive_policy)
sorx_target_defaults(sorx_adaptive_policy)
target_link_libraries(sorx_adaptive_policy PUBLIC sorx_core)

add_library(
    sorx_order_entry_profiles
    STATIC
        src/adapters/mockx_fix44.cpp)
add_library(sorx::order_entry_profiles ALIAS sorx_order_entry_profiles)
set_target_properties(
    sorx_order_entry_profiles
    PROPERTIES EXPORT_NAME order_entry_profiles)
sorx_target_defaults(sorx_order_entry_profiles)
target_link_libraries(sorx_order_entry_profiles PUBLIC sorx_core)

add_library(sorx_runtime STATIC src/runtime.cpp)
add_library(sorx::runtime ALIAS sorx_runtime)
set_target_properties(sorx_runtime PROPERTIES EXPORT_NAME runtime)
sorx_target_defaults(sorx_runtime)
target_link_libraries(
    sorx_runtime
    PUBLIC
        sorx_core
        sorx_config
        sorx_persistence
        Threads::Threads)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/feed_engine.cpp")
    add_library(sorx_md_core STATIC src/feed_engine.cpp)
    add_library(sorx::md_core ALIAS sorx_md_core)
    set_target_properties(sorx_md_core PROPERTIES EXPORT_NAME md_core)
    sorx_target_defaults(sorx_md_core)
    target_link_libraries(sorx_md_core PUBLIC sorx_core)
    target_link_libraries(sorx_runtime PUBLIC sorx_md_core)
endif()

if(TARGET sorx_md_core AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/mockx_md.cpp")
    add_library(
        sorx_mockx_md_adapter
        STATIC
            src/adapters/mockx_md.cpp
            src/adapters/mockx_md_session.cpp)
    add_library(sorx::mockx_md_adapter ALIAS sorx_mockx_md_adapter)
    set_target_properties(
        sorx_mockx_md_adapter
        PROPERTIES EXPORT_NAME mockx_md_adapter)
    sorx_target_defaults(sorx_mockx_md_adapter)
    target_link_libraries(
        sorx_mockx_md_adapter
        PUBLIC sorx_md_core
        PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(
            sorx_mockx_md_adapter
            PRIVATE ws2_32)
    endif()
endif()

if(SORX_ENABLE_QUICKFIX)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(quickfix CONFIG QUIET)
    foreach(quickfix_target IN ITEMS
            quickfix
            quickfix::quickfix
            QuickFIX::quickfix)
        if(TARGET ${quickfix_target} AND NOT TARGET QuickFIX::QuickFIX)
            add_library(QuickFIX::QuickFIX ALIAS ${quickfix_target})
        endif()
    endforeach()
    if(NOT TARGET QuickFIX::QuickFIX)
        find_package(QuickFIX REQUIRED MODULE)
    endif()

    add_library(
        sorx_quickfix_adapter
        STATIC
            src/adapters/quickfix_session.cpp)
    add_library(sorx::quickfix_adapter ALIAS sorx_quickfix_adapter)
    set_target_properties(
        sorx_quickfix_adapter
        PROPERTIES EXPORT_NAME quickfix_adapter)
    sorx_target_defaults(sorx_quickfix_adapter)
    target_link_libraries(
        sorx_quickfix_adapter
        PUBLIC
            sorx_order_entry_profiles
        PRIVATE
            QuickFIX::QuickFIX)
endif()

if(TARGET sorx_mockx_md_adapter AND
   TARGET sorx_quickfix_adapter AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/mockx_venue_runtime.cpp")
    add_library(
        sorx_mockx_venue_runtime
        STATIC
            src/adapters/mockx_venue_runtime.cpp)
    add_library(
        sorx::mockx_venue_runtime
        ALIAS sorx_mockx_venue_runtime)
    set_target_properties(
        sorx_mockx_venue_runtime
        PROPERTIES EXPORT_NAME mockx_venue_runtime)
    sorx_target_defaults(sorx_mockx_venue_runtime)
    target_link_libraries(
        sorx_mockx_venue_runtime
        PUBLIC
            sorx_runtime
            sorx_mockx_md_adapter
            sorx_quickfix_adapter
        PRIVATE
            Threads::Threads)
endif()

if(SORX_ENABLE_INTEGRATION)
    if(NOT TARGET sorx_md_core)
        message(FATAL_ERROR "The multi-MockX harness requires sorx_md_core")
    endif()
    add_executable(
        sorx_multi_mockx_demo
        examples/multi_mockx/main.cpp)
    sorx_target_defaults(sorx_multi_mockx_demo)
    target_link_libraries(
        sorx_multi_mockx_demo
        PRIVATE sorx_runtime sorx_order_entry_profiles)
endif()

if(SORX_BUILD_APPLIANCE)
    function(sorx_add_appliance_executable target source output_name)
        add_executable(${target} ${source})
        sorx_target_defaults(${target})
        target_link_libraries(${target} PRIVATE sorx_appliance)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name})
    endfunction()

    sorx_add_appliance_executable(sorxd apps/sorxd.cpp sorxd)
    sorx_add_appliance_executable(
        sorx_fixgw apps/sorx_fixgw.cpp sorx-fixgw)
    target_link_libraries(sorx_fixgw PRIVATE sorx_fixgw_core)
    sorx_add_appliance_executable(
        sorx_auditd apps/sorx_auditd.cpp sorx-auditd)
    sorx_add_appliance_executable(
        sorx_mgmtd apps/sorx_mgmtd.cpp sorx-mgmtd)
    sorx_add_appliance_executable(sorxctl apps/sorxctl.cpp sorxctl)
endif()

if(BUILD_TESTING)
    add_library(
        sorx_test_process_policy
        OBJECT tests/test_process_policy.cpp)
    sorx_target_defaults(sorx_test_process_policy)
endif()

function(sorx_add_test target source)
    add_executable(${target} ${source})
    target_sources(
        ${target}
        PRIVATE $<TARGET_OBJECTS:sorx_test_process_policy>)
    sorx_target_defaults(${target})
    sorx_enable_test_assertions(${target})
    target_link_libraries(${target} PRIVATE ${ARGN})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 120)
endfunction()

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME sorx_device_config_schema_contract
            COMMAND
                "${Python3_EXECUTABLE}"
                -X dev
                -W error
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_device_config_schema_contract.py"
                --self-test)
        set_tests_properties(
            sorx_device_config_schema_contract
            PROPERTIES TIMEOUT 30)
    else()
        message(
            STATUS
            "Python3 not found; DeviceConfigV2 schema drift test is disabled")
    endif()

    sorx_add_test(sorx_routing_tests tests/routing_tests.cpp sorx_core)
    sorx_add_test(
        sorx_allocation_property_tests
        tests/allocation_property_tests.cpp
        sorx_core)
    sorx_add_test(sorx_config_tests tests/config_tests.cpp sorx_config)
    sorx_add_test(sorx_order_core_tests tests/order_core_tests.cpp sorx_core)
    sorx_add_test(
        sorx_dispatch_lifecycle_tests
        tests/dispatch_lifecycle_tests.cpp
        sorx_core)
    sorx_add_test(
        sorx_risk_conservation_tests
        tests/risk_conservation_tests.cpp
        sorx_core)
    sorx_add_test(
        sorx_recovery_state_tests
        tests/recovery_state_tests.cpp
        sorx_core)
    sorx_add_test(sorx_transcript_tests tests/transcript_tests.cpp sorx_core)
    sorx_add_test(
        sorx_runtime_queue_tests
        tests/runtime_queue_tests.cpp
        sorx_runtime)
    sorx_add_test(
        sorx_runtime_tests
        tests/runtime_tests.cpp
        sorx_runtime)
    sorx_add_test(
        sorx_runtime_concurrency_tests
        tests/runtime_concurrency_tests.cpp
        sorx_runtime)
    set_tests_properties(
        sorx_runtime_concurrency_tests
        PROPERTIES TIMEOUT 60)
    sorx_add_test(
        sorx_crash_matrix_tests
        tests/crash_matrix_tests.cpp
        sorx_runtime)
    set_tests_properties(
        sorx_crash_matrix_tests
        PROPERTIES TIMEOUT 120)
    sorx_add_test(
        sorx_mockx_fix44_tests
        tests/mockx_fix44_tests.cpp
        sorx_order_entry_profiles)
    sorx_add_test(
        sorx_venue_conversion_tests
        tests/venue_conversion_tests.cpp
        sorx_order_entry_profiles)
    sorx_add_test(
        sorx_wire_identity_tests
        tests/wire_identity_tests.cpp
        sorx_runtime
        sorx_order_entry_profiles)
    sorx_add_test(
        sorx_journal_tests
        tests/journal_tests.cpp
        sorx_persistence)
    sorx_add_test(
        sorx_ha_foundation_tests
        tests/ha_foundation_tests.cpp
        sorx_ha)
    sorx_add_test(
        sorx_flight_recorder_foundation_tests
        tests/flight_recorder_foundation_tests.cpp
        sorx_recorder)
    sorx_add_test(
        sorx_appliance_protocol_tests
        tests/appliance_protocol_tests.cpp
        sorx_protocol
        sorx_appliance)
    sorx_add_test(
        sorx_appliance_cli_tests
        tests/appliance_cli_tests.cpp
        sorx_appliance)
    target_compile_definitions(
        sorx_appliance_cli_tests
        PRIVATE SORX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    sorx_add_test(
        sorx_session_auth_tests
        tests/session_auth_tests.cpp
        sorx_protocol)
    sorx_add_test(
        sorx_fixgw_translator_tests
        tests/fixgw_translator_tests.cpp
        sorx_fixgw_core)
    sorx_add_test(
        sorx_fastpath_contract_tests
        tests/fastpath_contract_tests.cpp
        sorx_fastpath)
    sorx_add_test(
        sorx_dispatch_pipeline_tests
        tests/dispatch_pipeline_tests.cpp
        sorx_fastpath)
    sorx_add_test(
        sorx_cancel_pipeline_tests
        tests/cancel_pipeline_tests.cpp
        sorx_fastpath)
    sorx_add_test(
        sorx_risk_dispatch_pipeline_tests
        tests/risk_dispatch_pipeline_tests.cpp
        sorx_risk_fastpath)
    sorx_add_test(
        sorx_native_protocol_codec_tests
        tests/native_protocol_codec_tests.cpp
        sorx_native_connectors
        sorx_recovery)
    sorx_add_test(
        sorx_native_marketdata_codec_tests
        tests/native_marketdata_codec_tests.cpp
        sorx_native_marketdata)
    sorx_add_test(
        sorx_fast_recovery_tests
        tests/fast_recovery_tests.cpp
        sorx_recovery)
    sorx_add_test(
        sorx_risk_dispatch_recovery_tests
        tests/risk_dispatch_recovery_tests.cpp
        sorx_recovery
        sorx_risk_fastpath)
    sorx_add_test(
        sorx_audit_collector_tests
        tests/audit_collector_tests.cpp
        sorx_audit)
    sorx_add_test(
        sorx_telemetry_snapshot_tests
        tests/telemetry_snapshot_tests.cpp
        sorx_telemetry)
    sorx_add_test(
        sorx_dataplane_foundation_tests
        tests/dataplane_foundation_tests.cpp
        sorx_dataplane)
    sorx_add_test(
        sorx_dataplane_equivalence_tests
        tests/dataplane_equivalence_tests.cpp
        sorx_dataplane)
    sorx_add_test(
        sorx_platform_readiness_tests
        tests/platform_readiness_tests.cpp
        sorx_platform)
    sorx_add_test(
        sorx_marketdata_foundation_tests
        tests/marketdata_foundation_tests.cpp
        sorx_marketdata)
    sorx_add_test(
        sorx_risk_kernel_foundation_tests
        tests/risk_kernel_foundation_tests.cpp
        sorx_risk)
    sorx_add_test(
        sorx_adaptive_policy_tests
        tests/adaptive_policy_tests.cpp
        sorx_adaptive_policy)
    sorx_add_test(
        sorx_adaptive_route_integration_tests
        tests/adaptive_route_integration_tests.cpp
        sorx_fastpath
        sorx_adaptive_policy)
    if(TARGET sorx_md_core AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/feed_engine_tests.cpp")
        sorx_add_test(
            sorx_feed_engine_tests
            tests/feed_engine_tests.cpp
            sorx_md_core)
    endif()
    if(TARGET sorx_mockx_md_adapter AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/mockx_md_tests.cpp")
        sorx_add_test(
            sorx_mockx_md_tests
            tests/mockx_md_tests.cpp
            sorx_mockx_md_adapter)
        sorx_add_test(
            sorx_mockx_md_session_tests
            tests/mockx_md_session_tests.cpp
            sorx_mockx_md_adapter)
        set_tests_properties(
            sorx_mockx_md_session_tests
            PROPERTIES TIMEOUT 120)
    endif()

    set(sorx_public_validation_libraries
        sorx_core
        sorx_config
        sorx_order_entry_profiles)
    if(TARGET sorx_md_core)
        list(APPEND sorx_public_validation_libraries sorx_md_core)
    endif()
    if(TARGET sorx_mockx_md_adapter)
        list(APPEND
            sorx_public_validation_libraries
            sorx_mockx_md_adapter)
    endif()
    sorx_add_test(
        sorx_public_validation_tests
        tests/public_validation_tests.cpp
        ${sorx_public_validation_libraries})
    if(TARGET sorx_md_core)
        target_compile_definitions(
            sorx_public_validation_tests
            PRIVATE SORX_PUBLIC_VALIDATION_HAS_MD_CORE=1)
    endif()

    if(SORX_ENABLE_QUICKFIX)
        sorx_add_test(
            sorx_quickfix_session_tests
            tests/quickfix_session_tests.cpp
            sorx_quickfix_adapter)
        sorx_add_test(
            sorx_quickfix_store_recovery_tests
            tests/quickfix_store_recovery_tests.cpp
            sorx_quickfix_adapter
            QuickFIX::QuickFIX)
        if(WIN32)
            target_link_libraries(
                sorx_quickfix_store_recovery_tests
                PRIVATE ws2_32)
        endif()
    endif()
    if(TARGET sorx_mockx_venue_runtime)
        sorx_add_test(
            sorx_mockx_venue_runtime_tests
            tests/mockx_venue_runtime_tests.cpp
            sorx_mockx_venue_runtime)
        set_tests_properties(
            sorx_mockx_venue_runtime_tests
            PROPERTIES TIMEOUT 120)
    endif()
    if(SORX_ENABLE_INTEGRATION)
        sorx_add_test(
            sorx_multi_mockx_tests
            tests/multi_mockx_tests.cpp
            sorx_runtime
            sorx_order_entry_profiles)
    endif()

    set(
        SORX_CORE_LINK_GRAPH_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/release-evidence/sorx_core_link_graph_$<CONFIG>.txt")
    set(SORX_CORE_LINK_GRAPH_CONTENT
"target=sorx_core
type=$<TARGET_PROPERTY:sorx_core,TYPE>
sources=$<JOIN:$<TARGET_PROPERTY:sorx_core,SOURCES>,|>
link_libraries=$<JOIN:$<TARGET_PROPERTY:sorx_core,LINK_LIBRARIES>,|>
interface_link_libraries=$<JOIN:$<TARGET_PROPERTY:sorx_core,INTERFACE_LINK_LIBRARIES>,|>
compile_features=$<JOIN:$<TARGET_PROPERTY:sorx_core,COMPILE_FEATURES>,|>
interface_compile_features=$<JOIN:$<TARGET_PROPERTY:sorx_core,INTERFACE_COMPILE_FEATURES>,|>
")
    file(
        GENERATE
        OUTPUT "${SORX_CORE_LINK_GRAPH_FILE}"
        CONTENT "${SORX_CORE_LINK_GRAPH_CONTENT}")
    add_test(
        NAME sorx_core_protocol_boundary
        COMMAND
            "${CMAKE_COMMAND}"
            "-DSORX_LINK_GRAPH_FILE=${SORX_CORE_LINK_GRAPH_FILE}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/core_protocol_boundary.cmake")
endif()

set(sorx_install_targets
    sorx_core
    sorx_config
    sorx_persistence
    sorx_ha
    sorx_recorder
    sorx_protocol
    sorx_fixgw_core
    sorx_appliance
    sorx_fastpath
    sorx_risk_fastpath
    sorx_native_connectors
    sorx_native_marketdata
    sorx_recovery
    sorx_audit
    sorx_telemetry
    sorx_dataplane
    sorx_platform
    sorx_marketdata
    sorx_risk
    sorx_adaptive_policy
    sorx_order_entry_profiles
    sorx_runtime)
if(TARGET sorx_md_core)
    list(APPEND sorx_install_targets sorx_md_core)
endif()
if(TARGET sorx_mockx_md_adapter)
    list(APPEND sorx_install_targets sorx_mockx_md_adapter)
endif()
if(TARGET sorx_quickfix_adapter)
    list(APPEND sorx_install_targets sorx_quickfix_adapter)
endif()
if(TARGET sorx_mockx_venue_runtime)
    list(APPEND sorx_install_targets sorx_mockx_venue_runtime)
endif()
if(SORX_BUILD_APPLIANCE)
    list(
        APPEND sorx_install_targets
        sorxd
        sorx_fixgw
        sorx_auditd
        sorx_mgmtd
        sorxctl)
endif()

install(
    TARGETS ${sorx_install_targets}
    EXPORT sorxTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING
    PATTERN "*.hpp")
install(
    DIRECTORY schemas/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sorx/schemas)
install(
    DIRECTORY deploy/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sorx/deploy)
install(
    DIRECTORY configs/appliance/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sorx/configs/appliance)
install(
    FILES
        README.md
        docs/APPLIANCE_IMPLEMENTATION.md
        docs/HARDWARE_QUALIFICATION.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sorx)
install(
    EXPORT sorxTargets
    FILE sorxTargets.cmake
    NAMESPACE sorx::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sorx)

set(SORX_PACKAGE_WITH_QUICKFIX ${SORX_ENABLE_QUICKFIX})
configure_package_config_file(
    cmake/sorxConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/sorxConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sorx)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/sorxConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/sorxConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/sorxConfigVersion.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindQuickFIX.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sorx)

if(BUILD_TESTING)
    get_property(SORX_TEST_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    add_test(
        NAME sorx_install_consumer_smoke
        COMMAND
            "${CMAKE_COMMAND}"
            "-DSORX_TEST_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DSORX_TEST_CONFIG=$<CONFIG>"
            "-DSORX_TEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DSORX_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DSORX_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            # Keep a single-config nested consumer on the exact compiler/SDK
            # architecture used to produce the installed libraries.
            "-DSORX_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DSORX_TEST_RC_COMPILER=${CMAKE_RC_COMPILER}"
            "-DSORX_TEST_MT=${CMAKE_MT}"
            "-DSORX_TEST_MULTI_CONFIG=${SORX_TEST_MULTI_CONFIG}"
            "-DSORX_TEST_DEPENDENCY_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
            "-DSORX_TEST_QUICKFIX_ROOT=${QUICKFIX_ROOT}"
            "-DSORX_TEST_QUICKFIX_DIR=${quickfix_DIR}"
            "-DSORX_TEST_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DSORX_TEST_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SorxInstallConsumerTest.cmake")
    set_tests_properties(sorx_install_consumer_smoke PROPERTIES TIMEOUT 180)
endif()
