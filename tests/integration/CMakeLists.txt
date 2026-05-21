# tests/integration/CMakeLists.txt

file(GLOB INTEGRATION_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
)

foreach(_src ${INTEGRATION_TEST_SOURCES})
    get_filename_component(_name "${_src}" NAME_WE)

    add_executable(${_name} "${_src}")
    target_include_directories(${_name}
        PRIVATE "${CMAKE_SOURCE_DIR}/src"
    )
    target_link_libraries(${_name}
        PRIVATE GTest::gtest_main
                hft_strategy
                hft_risk
                hft_oms
                hft_connectivity
                hft_position
                hft_infra
    )
    add_test(NAME ${_name} COMMAND ${_name})
    set_tests_properties(${_name} PROPERTIES TIMEOUT 60)

    message(STATUS "HFT Integration Test: registered '${_name}'")
endforeach()
