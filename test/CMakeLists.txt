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
)
target_link_libraries(ib_fix_tests PRIVATE ib_fix::fix_acceptor ib_fix::ib_bridge GTest::gtest_main)
target_compile_definitions(ib_fix_tests PRIVATE TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_output")
gtest_discover_tests(ib_fix_tests)
