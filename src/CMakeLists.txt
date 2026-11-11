# FIX 4.2 acceptor: the FIX side of the gateway, independent of Interactive Brokers
add_library(fix_acceptor
	fix/Fix42Codec.cpp
	fix/AcceptorSession.cpp
	fix/Acceptor.cpp
)
add_library(ib_fix::fix_acceptor ALIAS fix_acceptor)
target_include_directories(fix_acceptor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fix_acceptor PUBLIC fix8::FIX42)

# Interactive Brokers side: executes the FIX clients' orders through TWS
add_library(ib_bridge
	ib/IbTranslation.cpp
	ib/GatewayConnector.cpp
	ib/IbOrderHandler.cpp
	ib/TwsSettings.cpp
)
add_library(ib_fix::ib_bridge ALIAS ib_bridge)
target_include_directories(ib_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ib_bridge PUBLIC fix_acceptor ib_helper::ib_helper)

# the gateway engine: TWS connection + IB order handler + FIX acceptor, configured by EngineConfig
add_library(ib_fix_engine
	engine/EngineConfig.cpp
	engine/Engine.cpp
)
add_library(ib_fix::engine ALIAS ib_fix_engine)
target_include_directories(ib_fix_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ib_fix_engine PUBLIC ib_bridge)

add_executable(ib_fix_gateway main.cpp)
target_link_libraries(ib_fix_gateway PRIVATE ib_fix_engine)
