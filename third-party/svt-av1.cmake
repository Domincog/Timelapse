# Pinned official SVT-AV1 4.2.0 software encoder. Build the complete optimized
# encoder as a static library with the same /MT runtime as the app; no apps,
# tests, decoder or encoder DLL are distributed. See the SVT notices here.
# Both archives have pinned SHA-256 digests, including offline copies.
include(ExternalProject)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The AV1 encoder dependency requires MSVC for Windows x64.")
endif()
set(TIMELAPSE_SVT_AV1_ARCHIVE "" CACHE FILEPATH
    "Local SVT-AV1-v4.2.0.tar.gz matching the pinned SHA-256; empty downloads the official release")
set(TIMELAPSE_NASM_ARCHIVE "" CACHE FILEPATH
    "Local nasm-3.02-win64.zip matching the pinned SHA-256; empty downloads the official release")
set(timelapse_svt_source "https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v4.2.0/SVT-AV1-v4.2.0.tar.gz")
set(timelapse_nasm_source "https://www.nasm.us/pub/nasm/releasebuilds/3.02/win64/nasm-3.02-win64.zip")
if(TIMELAPSE_SVT_AV1_ARCHIVE)
    set(timelapse_svt_source "${TIMELAPSE_SVT_AV1_ARCHIVE}")
endif()
if(TIMELAPSE_NASM_ARCHIVE)
    set(timelapse_nasm_source "${TIMELAPSE_NASM_ARCHIVE}")
endif()
# Short paths also support older Windows SDK/MSBuild path limits.
set(timelapse_svt_root "${CMAKE_BINARY_DIR}/svt")
set(timelapse_svt_source_dir "${timelapse_svt_root}/s")
set(timelapse_svt_binary_dir "${timelapse_svt_root}/b")
set(timelapse_svt_output_dir "${timelapse_svt_root}/lib")
set(timelapse_nasm_dir "${timelapse_svt_root}/nasm")
ExternalProject_Add(timelapse_nasm
    URL "${timelapse_nasm_source}"
    URL_HASH SHA256=161d0bfaff53c2f9e9f3e69fd0672323ebabafd1268976a5cec11be92a19aee7
    DOWNLOAD_NO_PROGRESS ON
    PREFIX "${timelapse_svt_root}/np"
    SOURCE_DIR "${timelapse_nasm_dir}"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    LOG_DOWNLOAD ON
    LOG_OUTPUT_ON_FAILURE ON)
# Preserve the generator expression through the external project's own
# configuration instead of evaluating it in the outer multi-config build.
set(timelapse_svt_initial_cache "${timelapse_svt_root}/initial-cache.cmake")
file(WRITE "${timelapse_svt_initial_cache}"
    "set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>\" CACHE STRING \"\")\n")
set(timelapse_svt_build_type)
set(timelapse_svt_library "${timelapse_svt_output_dir}/$<CONFIG>/SvtAv1Enc.lib")
if(NOT CMAKE_CONFIGURATION_TYPES)
    set(timelapse_svt_build_type "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    set(timelapse_svt_library "${timelapse_svt_output_dir}/SvtAv1Enc.lib")
endif()
ExternalProject_Add(timelapse_svt_av1
    URL "${timelapse_svt_source}"
    URL_HASH SHA256=c7b13c4a84bd3751aa35fcc72be13e6875467e7c2216879251a486e5b1e4e740
    DOWNLOAD_NO_PROGRESS ON
    DEPENDS timelapse_nasm
    PREFIX "${timelapse_svt_root}/p"
    SOURCE_DIR "${timelapse_svt_source_dir}"
    BINARY_DIR "${timelapse_svt_binary_dir}"
    CMAKE_ARGS
        -C "${timelapse_svt_initial_cache}"
        ${timelapse_svt_build_type}
        "-DCMAKE_ASM_NASM_COMPILER=${timelapse_nasm_dir}/nasm.exe"
        "-DCMAKE_OUTPUT_DIRECTORY=${timelapse_svt_output_dir}"
        # SVT's variadic allocation macros require the conforming MSVC
        # preprocessor, including with the supported VS 2019 compiler.
        -DCMAKE_C_FLAGS=/Zc:preprocessor
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_APPS=OFF
        -DBUILD_TESTING=OFF
        -DCOMPILE_C_ONLY=OFF
        -DMINIMAL_BUILD=OFF
        -DRTC_BUILD=OFF
        -DLOG_QUIET=ON
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --target SvtAv1Enc --parallel
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${timelapse_svt_library}"
    LOG_DOWNLOAD ON
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON)
ExternalProject_Add_StepDependencies(timelapse_svt_av1 configure "${timelapse_svt_initial_cache}")
file(MAKE_DIRECTORY "${timelapse_svt_source_dir}/Source/API")
add_library(timelapse_svt STATIC IMPORTED GLOBAL)
set_target_properties(timelapse_svt PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${timelapse_svt_source_dir}/Source/API")
if(CMAKE_CONFIGURATION_TYPES)
    foreach(timelapse_config IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${timelapse_config}" timelapse_config_upper)
        set_property(TARGET timelapse_svt APPEND PROPERTY IMPORTED_CONFIGURATIONS "${timelapse_config_upper}")
        set_target_properties(timelapse_svt PROPERTIES
            "IMPORTED_LOCATION_${timelapse_config_upper}" "${timelapse_svt_output_dir}/${timelapse_config}/SvtAv1Enc.lib")
    endforeach()
else()
    set_target_properties(timelapse_svt PROPERTIES IMPORTED_LOCATION "${timelapse_svt_output_dir}/SvtAv1Enc.lib")
endif()
add_dependencies(timelapse_svt timelapse_svt_av1)
