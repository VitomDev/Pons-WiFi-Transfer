# Pons.exe, its core library and the tests. Use build.ps1, which also fetches
# the dependencies and builds the installer.
#   -DPONS_ZXING_SOURCE=<zxing-cpp source folder>
cmake_minimum_required(VERSION 3.21)
project(Pons CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no Visual C++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

# ZXing-C++ with the QR code writer only.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZXING_READERS OFF CACHE BOOL "" FORCE)
set(ZXING_WRITERS OLD CACHE STRING "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_DEPENDENCIES LOCAL CACHE STRING "" FORCE)
foreach(format 1D AZTEC DATAMATRIX MAXICODE PDF417)
    set(ZXING_ENABLE_${format} OFF CACHE BOOL "" FORCE)
endforeach()
set(ZXING_ENABLE_QRCODE ON CACHE BOOL "" FORCE)
add_subdirectory(${PONS_ZXING_SOURCE} zxing EXCLUDE_FROM_ALL)

# Pons' own code is held to stricter flags than the library.
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/W4 /WX /permissive- /sdl /utf-8 /guard:cf /external:anglebrackets /external:W0)
add_link_options(/guard:cf /CETCOMPAT /OPT:REF /OPT:ICF)

add_library(ponscore STATIC
    src/core/http.cpp
    src/core/network.cpp
    src/core/paths.cpp
    src/core/qr.cpp
    src/core/server.cpp
    src/core/settings.cpp
    src/core/text.cpp
    src/core/web_page.cpp
)
target_include_directories(ponscore PUBLIC src)
# ZXing's internal headers (the QR writer) are included as system headers.
target_include_directories(ponscore SYSTEM PRIVATE ${PONS_ZXING_SOURCE}/core/src)
target_link_libraries(ponscore PUBLIC ws2_32 iphlpapi bcrypt user32 advapi32 shell32 ole32 PRIVATE ZXing::ZXing)

add_executable(Pons WIN32
    src/app/main.cpp
    src/app/main_window.cpp
    src/app/theme.cpp
    src/app/Pons.rc
)
target_link_libraries(Pons PRIVATE ponscore gdi32 comctl32 dwmapi uxtheme)
# The manifest is embedded through Pons.rc.
target_link_options(Pons PRIVATE /MANIFEST:NO)

add_executable(PonsTests tests/tests.cpp)
target_link_libraries(PonsTests PRIVATE ponscore)
