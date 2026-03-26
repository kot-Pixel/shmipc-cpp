cmake_minimum_required(VERSION 3.14)
project(shmipc VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(SHMIPC_BUILD_SHARED  "Build as shared library instead of static" OFF)
option(SHMIPC_BUILD_EXAMPLES "Build example server/client programs"     ON)

set(SHMIPC_SOURCES
    src/ShmBufferManager.cpp
    src/ShmProtocolHandler.cpp
    src/ShmServer.cpp
    src/ShmServerSession.cpp
    src/ShmServerSessionManager.cpp
    src/ShmBenchTester.cpp
    src/ShmClientSession.cpp
    src/ShmClient.cpp
    src/ShareMemoryManager.cpp
    src/shmipc_api.cpp
)

if(SHMIPC_BUILD_SHARED)
    add_library(shmipc SHARED ${SHMIPC_SOURCES})
    # Tell our source files they are being compiled into a DLL/SO so that
    # SHMIPC_API expands to __declspec(dllexport) / visibility("default").
    target_compile_definitions(shmipc PRIVATE SHMIPC_BUILDING_DLL)
else()
    add_library(shmipc STATIC ${SHMIPC_SOURCES})
endif()

target_include_directories(shmipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(NOT ANDROID)
    target_link_libraries(shmipc PUBLIC pthread)
endif()

set_target_properties(shmipc PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
)

# ── Symbol visibility ────────────────────────────────────────────────────────
# Hide every C++ symbol by default; only SHMIPC_API-marked functions are
# exported.  This dramatically shrinks the .so export table and prevents
# accidental ABI leakage from internal C++ classes / STL types.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(shmipc PRIVATE
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
endif()

# ── Size optimisation for Android ───────────────────────────────────────────
# -Os  : optimise for code size (vs -O2 for speed)
# -fno-rtti : drop RTTI tables (safe — we never use dynamic_cast / typeid)
# -fno-unwind-tables / -fno-asynchronous-unwind-tables : drop unwind info that
#   the Android linker does not need at runtime (saves ~15-20% on ARM64)
if(ANDROID)
    target_compile_options(shmipc PRIVATE
        -Os
        -fno-rtti
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables
    )
    if(SHMIPC_BUILD_SHARED)
        # Remove unreferenced sections at link time
        target_link_options(shmipc PRIVATE
            -Wl,--gc-sections
            -Wl,--strip-all
        )
    endif()
endif()

if(SHMIPC_BUILD_EXAMPLES)
    add_executable(shmipc_server examples/server_main.c)
    target_link_libraries(shmipc_server PRIVATE shmipc)

    add_executable(shmipc_client examples/client_main.c)
    target_link_libraries(shmipc_client PRIVATE shmipc)
endif()

option(SHMIPC_BUILD_TESTS "Build benchmark / integration tests" ON)
if(SHMIPC_BUILD_TESTS)
    add_executable(shmipc_test1_s2c tests/test1_s2c.c)
    target_link_libraries(shmipc_test1_s2c PRIVATE shmipc pthread)

    add_executable(shmipc_test2_c2s tests/test2_c2s.c)
    target_link_libraries(shmipc_test2_c2s PRIVATE shmipc pthread)

    add_executable(shmipc_test3_duplex tests/test3_duplex.c)
    target_link_libraries(shmipc_test3_duplex PRIVATE shmipc pthread)
endif()

# ---------------------------------------------------------------
#  Install rules
#  cmake --install <builddir> --prefix <destdir>
#
#  Callers can control the library sub-directory via the standard
#  CMake variable CMAKE_INSTALL_LIBDIR (default: "lib").
#  The install.sh script passes -DCMAKE_INSTALL_LIBDIR=lib/<arch>
#  so each architecture lands in its own sub-directory.
#
#  Public C API headers only (shmipc.h + ShmConfig.h).
#  Internal C++ headers are implementation details and are not
#  installed — consumers only need the C API.
# ---------------------------------------------------------------
include(GNUInstallDirs)   # defines CMAKE_INSTALL_LIBDIR etc.

install(TARGETS shmipc
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"   COMPONENT lib
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"   COMPONENT lib
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"   COMPONENT lib
)

install(FILES
    include/shmipc/shmipc.h
    include/shmipc/ShmConfig.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/shmipc"
    COMPONENT   headers
)
