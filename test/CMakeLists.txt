# Links FIX8's FIX 4.2 library and ib_helper/TWS API into one program to prove they coexist
add_executable(build_smoke_test build_smoke_test.cpp)
target_link_libraries(build_smoke_test PRIVATE fix8::FIX42 ib_helper::ib_helper)
add_test(NAME build_smoke_test COMMAND build_smoke_test)

# ---------------------------------------------------------------------------------------------
# unit and integration tests
# ---------------------------------------------------------------------------------------------
if(NOT TARGET GTest::gtest_main)
	include(FetchContent)
	set(INSTALL_GTEST OFF)
	FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git
		GIT_TAG v1.18.0 GIT_SHALLOW ON)
	FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

add_executable(ib_fix_tests
	FixTestClient.cpp
	Fix42CodecTest.cpp
	AcceptorTest.cpp
	IbTranslationTest.cpp
	IbOrderHandlerTest.cpp
	GatewayTest.cpp
	TwsSettingsTest.cpp
	EngineTest.cpp
	OrderJournalTest.cpp
	PositionCheckTest.cpp
	TwsPositions.cpp
)
target_link_libraries(ib_fix_tests PRIVATE ib_fix::engine GTest::gtest_main)
target_compile_definitions(ib_fix_tests PRIVATE
	IB_FIX_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_output"
	TEST_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_resources")
gtest_discover_tests(ib_fix_tests)

# ---------------------------------------------------------------------------------------------
# live integration tests against TWS (paper trading); they place orders, so they are not run
# by ctest unless IB_FIX_TWS_TESTS is ON. See test_resources/README.md.
# ---------------------------------------------------------------------------------------------
option(IB_FIX_TWS_TESTS "Add the live TWS integration tests (they place paper orders) to ctest" OFF)
add_executable(ib_fix_tws_tests
	FixTestClient.cpp
	TcpProxy.cpp
	TwsPositions.cpp
	LivePositions.cpp
	TwsIntegrationTest.cpp
	TwsReconnectTest.cpp
)
target_link_libraries(ib_fix_tws_tests PRIVATE ib_fix::engine GTest::gtest_main)
target_compile_definitions(ib_fix_tws_tests PRIVATE
	TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_output"
	TEST_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_resources")
# read-only: prints the positions in a stock (default: the tests' symbol) in every account
add_executable(tws_positions tws_positions.cpp TwsPositions.cpp)
target_link_libraries(tws_positions PRIVATE ib_fix::ib_bridge)
target_compile_definitions(tws_positions PRIVATE TEST_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_resources")
if(IB_FIX_TWS_TESTS)
	add_test(NAME tws_integration COMMAND ib_fix_tws_tests)
	set_tests_properties(tws_integration PROPERTIES LABELS tws TIMEOUT 600)
endif()
