cmake_minimum_required(VERSION 3.22)

project(stereo_surround LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(game_speaker_processor STATIC
    src/game_speaker_processor.cpp
    src/game_speaker_processor.h
)
target_include_directories(game_speaker_processor PUBLIC src)

add_library(transaural_processor STATIC
    src/audio_frames.h
    src/transaural_processor.cpp
    src/transaural_processor.h
)
target_include_directories(transaural_processor PUBLIC src)
target_link_libraries(transaural_processor PUBLIC game_speaker_processor)
target_compile_definitions(transaural_processor PRIVATE NOMINMAX)

add_library(wasapi_runtime STATIC
    src/wasapi_bridge.cpp
    src/wasapi_bridge.h
)
target_include_directories(wasapi_runtime PUBLIC src)
target_link_libraries(wasapi_runtime PUBLIC
    transaural_processor Threads::Threads ole32 oleaut32 uuid propsys avrt
    ksuser comctl32 comdlg32 user32 gdi32
)
target_compile_definitions(wasapi_runtime PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WINVER=0x0A00 _WIN32_WINNT=0x0A00
)

add_executable(icon_generator tools/icon_generator.cpp)
set(generated_resource_dir "${CMAKE_CURRENT_BINARY_DIR}/generated-resources")
file(MAKE_DIRECTORY "${generated_resource_dir}")
set(app_icon "${generated_resource_dir}/stereo_surround.ico")
add_custom_command(
    OUTPUT "${app_icon}"
    COMMAND $<TARGET_FILE:icon_generator> "${app_icon}"
    DEPENDS icon_generator
    VERBATIM
)
add_custom_target(application_icon DEPENDS "${app_icon}")

set(APP_ICON_PATH "${app_icon}")
set(APP_FILE_DESCRIPTION "7.1 speaker virtualizer")
set(APP_INTERNAL_NAME "stereo_surround")
set(APP_ORIGINAL_FILENAME "stereo_surround.exe")
configure_file(resources/app.rc.in "${generated_resource_dir}/app.rc" @ONLY)
set_source_files_properties("${generated_resource_dir}/app.rc"
    PROPERTIES OBJECT_DEPENDS "${app_icon}")

add_executable(stereo_surround WIN32
    src/gui_main.cpp
    "${generated_resource_dir}/app.rc"
)
add_dependencies(stereo_surround application_icon)
target_include_directories(stereo_surround PRIVATE src)
target_link_libraries(stereo_surround PRIVATE wasapi_runtime)

if(MINGW)
    foreach(target IN ITEMS game_speaker_processor transaural_processor
                            wasapi_runtime icon_generator stereo_surround)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
    endforeach()
    target_link_options(stereo_surround PRIVATE
        -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections)
endif()

enable_testing()
add_executable(transaural_tests tests/transaural_tests.cpp)
target_link_libraries(transaural_tests PRIVATE transaural_processor)
add_executable(game_speaker_tests tests/game_speaker_tests.cpp)
target_link_libraries(game_speaker_tests PRIVATE transaural_processor)
if(MINGW)
    foreach(target IN ITEMS transaural_tests game_speaker_tests)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(${target} PRIVATE
            -static -static-libgcc -static-libstdc++)
    endforeach()
endif()
add_test(NAME transaural_tests COMMAND transaural_tests)
add_test(NAME game_speaker_tests COMMAND game_speaker_tests)
