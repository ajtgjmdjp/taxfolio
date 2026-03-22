cmake_minimum_required(VERSION 3.20)
project(taxfolio
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Tax-aware portfolio optimization engine"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
find_package(Eigen3 REQUIRED)
find_package(osqp REQUIRED)

# Core library
set(CORE_SOURCES
    core/models/position.cpp
    core/models/trade.cpp
    core/models/tax_policy.cpp
    core/optimizer/quadratic.cpp
    api.cpp
)

add_library(taxfolio_core STATIC ${CORE_SOURCES})
set_target_properties(taxfolio_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(taxfolio_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(taxfolio_core PUBLIC Eigen3::Eigen osqp::osqp)

# Linux needs explicit stdc++ linkage for std::variant across shared libraries
if(UNIX AND NOT APPLE)
    target_link_libraries(taxfolio_core PUBLIC stdc++)
endif()

# Python bindings
find_package(pybind11 CONFIG QUIET)
if(pybind11_FOUND)
    pybind11_add_module(_taxfolio python/bindings.cpp)
    target_link_libraries(_taxfolio PRIVATE taxfolio_core)
    install(TARGETS _taxfolio DESTINATION taxfolio)
endif()

# Tests
option(TAXFOLIO_BUILD_TESTS "Build tests" ON)
if(TAXFOLIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
