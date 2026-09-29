# Generate the real engine with allocation-scope markers only. Keep all hooks
# out of production and fail configuration if source edits invalidate an anchor.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine.cpp")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/engine.cpp" engine_source)

function(lapse_startup_insert_unique needle replacement)
    string(FIND "${engine_source}" "${needle}" first)
    if(first EQUAL -1)
        message(FATAL_ERROR "Startup test instrumentation anchor is absent: ${needle}")
    endif()
    string(LENGTH "${needle}" needle_length)
    math(EXPR after_first "${first} + ${needle_length}")
    string(SUBSTRING "${engine_source}" ${after_first} -1 remaining)
    string(FIND "${remaining}" "${needle}" duplicate)
    if(NOT duplicate EQUAL -1)
        message(FATAL_ERROR "Startup test instrumentation anchor is duplicated: ${needle}")
    endif()
    string(REPLACE "${needle}" "${replacement}" engine_source "${engine_source}")
    set(engine_source "${engine_source}" PARENT_SCOPE)
endfunction()

lapse_startup_insert_unique("snapshot.emplace(settings_);"
    "{ probe::Scope scope(probe::Stage::Snapshot, start_); snapshot.emplace(settings_); }")
lapse_startup_insert_unique("session.emplace(cfg);"
    "{ probe::Scope scope(probe::Stage::Session); session.emplace(cfg); }")
lapse_startup_insert_unique("if (!camera) camera.emplace();"
    "if (!camera) { probe::Scope scope(probe::Stage::Camera); camera.emplace(); }")
lapse_startup_insert_unique("if (!encoder) encoder.emplace();"
    "if (!encoder) { probe::Scope scope(probe::Stage::Encoder); encoder.emplace(); }")
lapse_startup_insert_unique("auto closeRecording = [&](const std::wstring& reason) {"
    "auto closeRecording = [&](const std::wstring& reason) { probe::Scope scope(probe::Stage::Close);")

set(startup_fixture_directory "${CMAKE_CURRENT_BINARY_DIR}/engine-startup-fixture")
file(MAKE_DIRECTORY "${startup_fixture_directory}")
file(WRITE "${startup_fixture_directory}/engine_startup_instrumented.cpp" "${engine_source}")
unset(engine_source)
