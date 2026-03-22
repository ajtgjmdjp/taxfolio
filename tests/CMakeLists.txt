find_package(GTest REQUIRED)

add_executable(taxfolio_tests
    cpp/test_api.cpp
    cpp/test_tax_lot.cpp
    cpp/test_tax_policy.cpp
)

target_link_libraries(taxfolio_tests
    PRIVATE taxfolio_core GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(taxfolio_tests)
