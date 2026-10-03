# Pinned software AV1 encoder: libaom (BSD-2-Clause and AOMedia Patent License
# 1.0; see libaom-LICENSE.txt and libaom-PATENTS.txt). libaom's configure step
# rewrites global CMAKE_<LANG>_FLAGS_<CONFIG> cache entries, so it is built as
# an isolated external project and must never be added with add_subdirectory().
# Only the 8-bit encoder library is built: no decoder, apps, tests or docs.
#
# Both downloads are verified against pinned SHA-256 digests. For an offline
# build, set TIMELAPSE_LIBAOM_ARCHIVE and TIMELAPSE_NASM_ARCHIVE to local copies
# of the same files; they are verified identically. libaom also needs Perl to
# generate its CPU-dispatch headers; Git for Windows includes one.
include(ExternalProject)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The AV1 encoder dependency requires MSVC for Windows x64.")
endif()

set(TIMELAPSE_LIBAOM_ARCHIVE "" CACHE FILEPATH
    "Local libaom-3.15.1.tar.gz matching the pinned SHA-256; empty downloads the official release")
set(TIMELAPSE_NASM_ARCHIVE "" CACHE FILEPATH
    "Local nasm-3.02-win64.zip matching the pinned SHA-256; empty downloads the official release")
set(timelapse_libaom_source "https://storage.googleapis.com/aom-releases/libaom-3.15.1.tar.gz")
set(timelapse_nasm_source "https://www.nasm.us/pub/nasm/releasebuilds/3.02/win64/nasm-3.02-win64.zip")
if(TIMELAPSE_LIBAOM_ARCHIVE)
    set(timelapse_libaom_source "${TIMELAPSE_LIBAOM_ARCHIVE}")
endif()
if(TIMELAPSE_NASM_ARCHIVE)
    set(timelapse_nasm_source "${TIMELAPSE_NASM_ARCHIVE}")
endif()

find_package(Git QUIET)
set(timelapse_perl_hints)
if(GIT_EXECUTABLE)
    # git.exe lives in Git/cmd, Git/bin or Git/mingw64/bin; Perl is in Git/usr/bin.
    get_filename_component(timelapse_git_bin "${GIT_EXECUTABLE}" DIRECTORY)
    list(APPEND timelapse_perl_hints "${timelapse_git_bin}/../usr/bin" "${timelapse_git_bin}/../../usr/bin")
endif()
find_program(TIMELAPSE_PERL_EXECUTABLE perl HINTS ${timelapse_perl_hints}
    DOC "Perl used only to generate libaom's CPU-dispatch headers")
if(NOT TIMELAPSE_PERL_EXECUTABLE)
    message(FATAL_ERROR "Perl is required to build the AV1 encoder. Install Git for Windows, "
        "which includes Perl, or set TIMELAPSE_PERL_EXECUTABLE.")
endif()

# MSBuild's intermediate paths inside libaom are long; keep this prefix short.
set(timelapse_aom_root "${CMAKE_BINARY_DIR}/aom")
set(timelapse_aom_source_dir "${timelapse_aom_root}/s")
set(timelapse_aom_binary_dir "${timelapse_aom_root}/b")
set(timelapse_nasm_dir "${timelapse_aom_root}/nasm")

ExternalProject_Add(timelapse_nasm
    URL "${timelapse_nasm_source}"
    URL_HASH SHA256=161d0bfaff53c2f9e9f3e69fd0672323ebabafd1268976a5cec11be92a19aee7
    DOWNLOAD_NO_PROGRESS ON
    PREFIX "${timelapse_aom_root}/np"
    SOURCE_DIR "${timelapse_nasm_dir}"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    LOG_DOWNLOAD ON
    LOG_OUTPUT_ON_FAILURE ON)

# The runtime selection must reach libaom's own multi-config generator
# unevaluated, so it goes through an initial cache rather than CMAKE_ARGS.
set(timelapse_aom_initial_cache "${timelapse_aom_root}/initial-cache.cmake")
file(WRITE "${timelapse_aom_initial_cache}"
    "set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>\" CACHE STRING \"\")\n")
set(timelapse_aom_build_type)
set(timelapse_aom_library "${timelapse_aom_binary_dir}/$<CONFIG>/aom.lib")
if(NOT CMAKE_CONFIGURATION_TYPES)
    set(timelapse_aom_build_type "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    set(timelapse_aom_library "${timelapse_aom_binary_dir}/aom.lib")
endif()

ExternalProject_Add(timelapse_libaom
    URL "${timelapse_libaom_source}"
    URL_HASH SHA256=8ca0c52746174603500f0adb6f2a215d69c9ca2aab2acb3caa06fb791d8d01bf
    DOWNLOAD_NO_PROGRESS ON
    DEPENDS timelapse_nasm
    PREFIX "${timelapse_aom_root}/p"
    SOURCE_DIR "${timelapse_aom_source_dir}"
    BINARY_DIR "${timelapse_aom_binary_dir}"
    CMAKE_ARGS
        -C "${timelapse_aom_initial_cache}"
        ${timelapse_aom_build_type}
        "-DCMAKE_ASM_NASM_COMPILER=${timelapse_nasm_dir}/nasm.exe"
        "-DPERL_EXECUTABLE=${TIMELAPSE_PERL_EXECUTABLE}"
        # Windows SDK 10.0.19041 headers trigger C5105 under libaom's /std:c11.
        -DAOM_EXTRA_C_FLAGS=/wd5105
        -DCONFIG_AV1_DECODER=0
        -DCONFIG_AV1_HIGHBITDEPTH=0
        -DCONFIG_DENOISE=0
        -DCONFIG_LIBYUV=0
        -DCONFIG_WEBM_IO=0
        -DENABLE_APPS=0
        -DENABLE_DOCS=0
        -DENABLE_EXAMPLES=0
        -DENABLE_TESTDATA=0
        -DENABLE_TESTS=0
        -DENABLE_TOOLS=0
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --target aom --parallel
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${timelapse_aom_library}"
    LOG_DOWNLOAD ON
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON)
ExternalProject_Add_StepDependencies(timelapse_libaom configure "${timelapse_aom_initial_cache}")

# Imported include directories must exist when the build system is generated.
file(MAKE_DIRECTORY "${timelapse_aom_source_dir}")
add_library(timelapse_aom STATIC IMPORTED GLOBAL)
set_target_properties(timelapse_aom PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${timelapse_aom_source_dir}")
if(CMAKE_CONFIGURATION_TYPES)
    foreach(timelapse_config IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${timelapse_config}" timelapse_config_upper)
        set_property(TARGET timelapse_aom APPEND PROPERTY IMPORTED_CONFIGURATIONS "${timelapse_config_upper}")
        set_target_properties(timelapse_aom PROPERTIES
            "IMPORTED_LOCATION_${timelapse_config_upper}" "${timelapse_aom_binary_dir}/${timelapse_config}/aom.lib")
    endforeach()
else()
    set_target_properties(timelapse_aom PROPERTIES IMPORTED_LOCATION "${timelapse_aom_binary_dir}/aom.lib")
endif()
add_dependencies(timelapse_aom timelapse_libaom)
