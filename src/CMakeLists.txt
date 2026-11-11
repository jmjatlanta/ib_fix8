# FIX 4.2 acceptor: the FIX side of the gateway, independent of Interactive Brokers
add_library(fix_acceptor
	fix/Fix42Codec.cpp
	fix/AcceptorSession.cpp
	fix/Acceptor.cpp
)
add_library(ib_fix::fix_acceptor ALIAS fix_acceptor)
target_include_directories(fix_acceptor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fix_acceptor PUBLIC fix8::FIX42)
