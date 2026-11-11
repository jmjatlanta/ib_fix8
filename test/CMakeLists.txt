# Links FIX8's FIX 4.2 library and ib_helper/TWS API into one program to prove they coexist
add_executable(build_smoke_test build_smoke_test.cpp)
target_link_libraries(build_smoke_test PRIVATE fix8::FIX42 ib_helper::ib_helper)
add_test(NAME build_smoke_test COMMAND build_smoke_test)
