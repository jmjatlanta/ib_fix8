cmake_minimum_required(VERSION 3.25)
project(ib_fix
	VERSION 0.1.0
	DESCRIPTION "FIX 4.2 front end for Interactive Brokers Trader Workstation"
	LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT is_multi_config AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

foreach(required external/fix8/CMakeLists.txt external/ib_helper/CMakeLists.txt
		external/ib_helper/IntelDFP/CMakeLists.txt)
	if(NOT EXISTS ${PROJECT_SOURCE_DIR}/${required})
		message(FATAL_ERROR "${required} not found: run 'git submodule update --init --recursive'")
	endif()
endforeach()

# ---------------------------------------------------------------------------------------------
# Interactive Brokers TWS API and ib_helper
# ---------------------------------------------------------------------------------------------
set(IB_CLIENT_SOURCE_DIR "$ENV{HOME}/IBJts/source" CACHE PATH
	"TWS API source directory (contains CppClient/ and proto/)")
if(NOT EXISTS ${IB_CLIENT_SOURCE_DIR}/CppClient/client/EWrapper.h)
	message(FATAL_ERROR "Cannot find CppClient/client/EWrapper.h in IB_CLIENT_SOURCE_DIR=${IB_CLIENT_SOURCE_DIR}")
endif()

# Apple's libc++ has no std::chrono time zones; ib_helper falls back to Howard Hinnant's date library
if(APPLE)
	set(hh_datelib_default ON)
else()
	set(hh_datelib_default OFF)
endif()
option(HH_DATELIB "Use Howard Hinnant's date library (needed where std::chrono lacks time zones)" ${hh_datelib_default})

# provides ib_helper::ib_helper and ib_helper::ib_api (the TWS API client); its unit tests are
# off by default as a subproject (IB_HELPER_BUILD_TESTS)
add_subdirectory(external/ib_helper)

# ---------------------------------------------------------------------------------------------
# FIX8 (only the FIX 4.2 schema library is needed)
# ---------------------------------------------------------------------------------------------
set(FIX8_SCHEMAS FIX42)
add_subdirectory(external/fix8)

# ---------------------------------------------------------------------------------------------
enable_testing()
add_subdirectory(test)
