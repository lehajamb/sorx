cmake_minimum_required(VERSION 3.20)

get_filename_component(
    SORX_SOURCE_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.."
    ABSOLUTE)

# Keep this list aligned with the protocol-neutral sorx_core target, its public
# value contracts, and the normalized order-entry contract consumed by core.
# Runtime, persistence, feed profiles, and venue adapters are intentionally
# separate ownership boundaries.
set(SORX_CORE_FILES
    "include/sorx/types.hpp"
    "include/sorx/book_view.hpp"
    "include/sorx/route_rib.hpp"
    "include/sorx/routing_policy.hpp"
    "include/sorx/adapters/order_entry.hpp"
    "include/sorx/order_core.hpp"
    "include/sorx/transcript.hpp"
    "src/route_rib.cpp"
    "src/routing_policy.cpp"
    "src/order_core.cpp"
    "src/transcript.cpp")

set(SORX_CORE_TARGET_SOURCES
    "src/route_rib.cpp"
    "src/routing_policy.cpp"
    "src/order_core.cpp"
    "src/transcript.cpp")

# Patterns use identifier boundaries so legitimate neutral names such as
# Stitching and KillSwitch are not mistaken for ITCH or switch/socket APIs.
set(SORX_FORBIDDEN_PATTERNS
    "(^|[^a-z0-9_])mockx([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])quickfix([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])fix[0-9]*([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])mold(udp64)?([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])soup(bin)?([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])glimpse([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])itch([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])socket([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])sockaddr([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])winsock([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])wsasocket([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])sendto([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])recvfrom([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])udp([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])tcp([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])asio([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])thread([^a-z0-9_]|$)"
    "(^|[^a-z0-9_])jthread([^a-z0-9_]|$)")

foreach(relative_path IN LISTS SORX_CORE_FILES)
    set(absolute_path "${SORX_SOURCE_ROOT}/${relative_path}")
    if(NOT EXISTS "${absolute_path}")
        message(FATAL_ERROR
            "Core protocol-boundary input is missing: ${relative_path}")
    endif()

    file(READ "${absolute_path}" source_text)
    string(TOLOWER "${source_text}" lower_source_text)
    foreach(pattern IN LISTS SORX_FORBIDDEN_PATTERNS)
        string(REGEX MATCH "${pattern}" forbidden_match "${lower_source_text}")
        if(NOT forbidden_match STREQUAL "")
            string(REPLACE "\n" " " printable_match "${forbidden_match}")
            message(FATAL_ERROR
                "Protocol/network boundary violation in ${relative_path}: "
                "'${printable_match}' matched '${pattern}'")
        endif()
    endforeach()
endforeach()

if(NOT DEFINED SORX_LINK_GRAPH_FILE OR
   NOT EXISTS "${SORX_LINK_GRAPH_FILE}")
    message(FATAL_ERROR
        "Generated sorx_core dependency graph is missing: "
        "${SORX_LINK_GRAPH_FILE}")
endif()

file(STRINGS "${SORX_LINK_GRAPH_FILE}" graph_lines)
foreach(graph_line IN LISTS graph_lines)
    if(NOT graph_line MATCHES "^([a-z_]+)=(.*)$")
        message(FATAL_ERROR
            "Malformed sorx_core dependency graph line: ${graph_line}")
    endif()
    set("SORX_GRAPH_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
endforeach()

if(NOT SORX_GRAPH_target STREQUAL "sorx_core" OR
   NOT SORX_GRAPH_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR
        "Unexpected generated core target identity/type: "
        "${SORX_GRAPH_target}/${SORX_GRAPH_type}")
endif()

# sorx_core is deliberately self-contained: the C++ standard library and its
# own headers/sources need no CMake link item. Any link dependency here is a
# release-boundary expansion and must be reviewed explicitly.
foreach(graph_property IN ITEMS link_libraries interface_link_libraries)
    if(NOT SORX_GRAPH_${graph_property} STREQUAL "")
        message(FATAL_ERROR
            "sorx_core has a forbidden ${graph_property} dependency: "
            "${SORX_GRAPH_${graph_property}}")
    endif()
endforeach()

set(graph_sources "${SORX_GRAPH_sources}")
string(REPLACE "|" ";" graph_sources "${graph_sources}")
list(LENGTH graph_sources graph_source_count)
list(LENGTH SORX_CORE_TARGET_SOURCES expected_source_count)
if(NOT graph_source_count EQUAL expected_source_count)
    message(FATAL_ERROR
        "sorx_core source graph has ${graph_source_count} entries; "
        "expected ${expected_source_count}: ${SORX_GRAPH_sources}")
endif()

foreach(graph_source IN LISTS graph_sources)
    file(TO_CMAKE_PATH "${graph_source}" normalized_graph_source)
    if(IS_ABSOLUTE "${normalized_graph_source}")
        file(
            RELATIVE_PATH
            normalized_graph_source
            "${SORX_SOURCE_ROOT}"
            "${normalized_graph_source}")
    endif()
    list(FIND SORX_CORE_TARGET_SOURCES "${normalized_graph_source}" source_index)
    if(source_index EQUAL -1)
        message(FATAL_ERROR
            "Generated sorx_core graph contains an unscanned source: "
            "${normalized_graph_source}")
    endif()
endforeach()

if(NOT SORX_GRAPH_compile_features MATCHES "(^|\\|)cxx_std_20(\\||$)" OR
   NOT SORX_GRAPH_interface_compile_features MATCHES
       "(^|\\|)cxx_std_20(\\||$)")
    message(FATAL_ERROR
        "sorx_core must publish and consume cxx_std_20; generated features: "
        "${SORX_GRAPH_compile_features} / "
        "${SORX_GRAPH_interface_compile_features}")
endif()

list(LENGTH SORX_CORE_FILES checked_file_count)
message(STATUS
    "SORX core protocol/dependency boundary clean "
    "(${checked_file_count} files and ${graph_source_count} target sources checked)")
