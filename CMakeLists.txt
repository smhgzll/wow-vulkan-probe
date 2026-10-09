cmake_minimum_required(VERSION 3.20)
project(vulkan_depth_probe LANGUAGES CXX)

find_path(VULKAN_INCLUDE_DIR
    NAMES vulkan/vk_layer.h
    REQUIRED
)

find_package(Threads REQUIRED)

set(DEPTH_PROBE_LAYER_SOURCES
    layer.cpp
    src/layer_state.cpp
    src/depth_tracking.cpp
    src/depth_selection.cpp
    src/swapchain_tracking.cpp
    src/present.cpp
    src/overlay_renderer.cpp
    src/overlay_pipeline.cpp
    src/scene_color.cpp
    src/draw_probe.cpp
    src/pipeline_discovery.cpp
    src/discovery_spirv.cpp
    src/discovery_samplers.cpp
    src/discovery_resources.cpp
    src/inspector_memory.cpp
    src/water_reflections.cpp
    src/water_resources.cpp
    src/water_capture.cpp
    src/water_spirv.cpp
    src/water_surface.cpp
    src/water_shading.cpp
    src/inspector_uniform_gpu.cpp
    src/discovery_commands.cpp
    src/discovery_capture.cpp
    src/discovery_ui.cpp
    src/inspector_replay.cpp
    src/inspector_ui.cpp
    src/inspector_gpu.cpp
    src/inspector_assets.cpp
    src/matrix_analyzer.cpp
    src/camera_math.cpp
    src/camera_analyzer.cpp
    src/process_memory.cpp
    src/memory_camera.cpp
    src/memory_camera_scan.cpp
    src/memory_sun.cpp
    src/depth_reconstruction.cpp
    src/fog_math.cpp
    src/draw_semantics.cpp
    src/draw_classifier.cpp
    src/game_lights.cpp
    src/game_lights_capture.cpp
    src/game_fog.cpp
    src/game_pcf.cpp
    src/spirv_soft_shadows.cpp
    src/game_soft_shadows.cpp
    src/game_fog_capture.cpp
    src/draw_segments.cpp
    src/volumetric_fog.cpp
    src/scene_debugger.cpp
    src/gpu_diagnostics.cpp
    src/overlay_dispatch.cpp
    src/overlay_sync.cpp
    src/sync_tracking.cpp
    src/sync_capabilities.cpp
    src/shader_resources.cpp
    src/heartbeat.cpp
    src/overlay_settings.cpp
    src/overlay_uniform.cpp
    src/overlay_menu.cpp
    src/overlay_input.cpp
    src/camera_hud_input.cpp
)

add_library(VkLayer_depth_probe SHARED ${DEPTH_PROBE_LAYER_SOURCES})

target_compile_features(VkLayer_depth_probe PRIVATE cxx_std_20)
target_include_directories(VkLayer_depth_probe PRIVATE "${VULKAN_INCLUDE_DIR}")
target_link_libraries(VkLayer_depth_probe PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

target_compile_options(VkLayer_depth_probe PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(VkLayer_depth_probe PROPERTIES
    OUTPUT_NAME "VkLayer_depth_probe"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/VK_LAYER_depth_probe.json"
    "${CMAKE_CURRENT_BINARY_DIR}/VK_LAYER_depth_probe.json"
    COPYONLY
)

# Opt-in: builds a CPU-only fake Vulkan driver to exercise ownership/failure paths.
option(DEPTH_PROBE_BUILD_TESTS "Build layer lifecycle regression tests" OFF)
if(DEPTH_PROBE_BUILD_TESTS)
    enable_testing()
    add_executable(lifecycle_test tests/lifecycle_test.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(lifecycle_test PRIVATE cxx_std_20)
    target_include_directories(lifecycle_test PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(lifecycle_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(lifecycle_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(lifecycle_test PRIVATE
        DEPTH_PROBE_TEST_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    add_executable(settings_test tests/settings_test.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(settings_test PRIVATE cxx_std_20)
    target_include_directories(settings_test PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(settings_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(settings_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME overlay_settings COMMAND settings_test)
    add_test(NAME overlay_input_fallback COMMAND settings_test --input-fallback)
    set_tests_properties(overlay_settings PROPERTIES ENVIRONMENT "DEPTH_PROBE_UI_INPUT=0")
    # Private fake libxcb exercises the real input worker without touching a display.
    find_path(XCB_INCLUDE_DIR NAMES xcb/xcb.h)
    if(XCB_INCLUDE_DIR)
        add_library(fake_xcb SHARED tests/fake_xcb.cpp)
        target_compile_features(fake_xcb PRIVATE cxx_std_20)
        target_include_directories(fake_xcb PRIVATE "${XCB_INCLUDE_DIR}")
        target_link_libraries(fake_xcb PRIVATE Threads::Threads)
        set_target_properties(fake_xcb PROPERTIES OUTPUT_NAME xcb SOVERSION 1
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-xcb")
        add_executable(input_backend_test tests/input_backend_test.cpp ${DEPTH_PROBE_LAYER_SOURCES})
        target_compile_features(input_backend_test PRIVATE cxx_std_20)
        target_include_directories(input_backend_test PRIVATE src "${VULKAN_INCLUDE_DIR}" "${XCB_INCLUDE_DIR}")
        target_link_libraries(input_backend_test PRIVATE fake_xcb Threads::Threads ${CMAKE_DL_LIBS})
        target_compile_options(input_backend_test PRIVATE -Wall -Wextra -Wpedantic)
        foreach(mode RANGE 0 3)
            add_test(NAME overlay_input_grabs_${mode} COMMAND input_backend_test ${mode})
        endforeach()
        add_test(NAME overlay_input_keyboard_fallback COMMAND input_backend_test 0 no-shared-pointer)
        add_test(NAME inspector_input_shared COMMAND input_backend_test 0 inspector)
        add_test(NAME inspector_input_captured COMMAND input_backend_test 3 inspector)
    endif()
    add_executable(vulkan_smoke tests/vulkan_smoke.cpp)
    target_compile_features(vulkan_smoke PRIVATE cxx_std_20)
    target_include_directories(vulkan_smoke PRIVATE "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(vulkan_smoke PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(vulkan_smoke PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(runtime_vulkan_smoke tests/runtime_vulkan_smoke.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(runtime_vulkan_smoke PRIVATE cxx_std_20)
    target_include_directories(runtime_vulkan_smoke PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(runtime_vulkan_smoke PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(runtime_vulkan_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(runtime_vulkan_smoke PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # Run manually with an explicit ICD: this test needs a usable Vulkan driver.
    add_test(NAME layer_lifecycle COMMAND lifecycle_test)
    add_test(NAME depth_diagnostics COMMAND lifecycle_test --probe)
    add_executable(discovery_test tests/discovery_test.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(discovery_test PRIVATE cxx_std_20)
    target_include_directories(discovery_test PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(discovery_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(discovery_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(discovery_test PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME pipeline_discovery COMMAND discovery_test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME discovery_report COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery_report_test.py")
        add_test(NAME reflection_input_report COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/reflection_input_report_test.py")
    endif()
    add_executable(discovery_vulkan_smoke tests/discovery_vulkan_smoke.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(discovery_vulkan_smoke PRIVATE cxx_std_20)
    target_include_directories(discovery_vulkan_smoke PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(discovery_vulkan_smoke PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(discovery_vulkan_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(discovery_vulkan_smoke PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(inspector_test tests/inspector_test.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(inspector_test PRIVATE cxx_std_20)
    target_include_directories(inspector_test PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(inspector_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(inspector_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(inspector_test PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME surface_inspector COMMAND inspector_test)
    add_executable(inspector_vulkan_smoke tests/inspector_vulkan_smoke.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(inspector_vulkan_smoke PRIVATE cxx_std_20)
    target_include_directories(inspector_vulkan_smoke PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(inspector_vulkan_smoke PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(inspector_vulkan_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(inspector_vulkan_smoke PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(hook_bench tests/hook_bench.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(hook_bench PRIVATE cxx_std_20)
    target_include_directories(hook_bench PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(hook_bench PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_executable(water_spirv_test tests/water_spirv_test.cpp src/water_spirv.cpp)
    target_compile_features(water_spirv_test PRIVATE cxx_std_20)
    target_include_directories(water_spirv_test PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(water_spirv_test PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(water_spirv_test PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME water_spirv COMMAND water_spirv_test)
    add_executable(matrix_analyzer_test tests/matrix_analyzer_test.cpp src/matrix_analyzer.cpp)
    target_compile_features(matrix_analyzer_test PRIVATE cxx_std_20)
    target_include_directories(matrix_analyzer_test PRIVATE src)
    add_test(NAME matrix_analyzer COMMAND matrix_analyzer_test)
    add_executable(camera_math_test tests/camera_math_test.cpp src/camera_math.cpp src/matrix_analyzer.cpp)
    target_compile_features(camera_math_test PRIVATE cxx_std_20)
    target_include_directories(camera_math_test PRIVATE src)
    add_test(NAME camera_math COMMAND camera_math_test)
    add_executable(camera_analyzer_test tests/camera_analyzer_test.cpp src/camera_analyzer.cpp src/matrix_analyzer.cpp)
    target_compile_features(camera_analyzer_test PRIVATE cxx_std_20)
    target_include_directories(camera_analyzer_test PRIVATE src)
    add_test(NAME camera_analyzer COMMAND camera_analyzer_test)
    add_executable(memory_camera_test tests/memory_camera_test.cpp src/memory_camera.cpp src/memory_camera_scan.cpp src/process_memory.cpp src/camera_math.cpp src/matrix_analyzer.cpp)
    target_compile_features(memory_camera_test PRIVATE cxx_std_20)
    target_include_directories(memory_camera_test PRIVATE src)
    target_link_libraries(memory_camera_test PRIVATE Threads::Threads)
    add_test(NAME memory_camera COMMAND memory_camera_test)
    add_executable(memory_sun_test tests/memory_sun_test.cpp src/memory_sun.cpp src/process_memory.cpp)
    target_compile_features(memory_sun_test PRIVATE cxx_std_20)
    target_include_directories(memory_sun_test PRIVATE src)
    add_test(NAME memory_sun COMMAND memory_sun_test)
    add_executable(depth_reconstruction_test tests/depth_reconstruction_test.cpp src/depth_reconstruction.cpp src/memory_camera.cpp src/process_memory.cpp src/camera_math.cpp src/matrix_analyzer.cpp)
    target_compile_features(depth_reconstruction_test PRIVATE cxx_std_20)
    target_include_directories(depth_reconstruction_test PRIVATE src)
    add_test(NAME depth_reconstruction COMMAND depth_reconstruction_test)
    add_executable(water_surface_test tests/water_surface_test.cpp src/water_surface.cpp src/water_shading.cpp)
    target_compile_features(water_surface_test PRIVATE cxx_std_20)
    target_include_directories(water_surface_test PRIVATE src)
    add_test(NAME water_surface COMMAND water_surface_test)
    add_executable(fog_math_test tests/fog_math_test.cpp src/fog_math.cpp)
    target_compile_features(fog_math_test PRIVATE cxx_std_20)
    target_include_directories(fog_math_test PRIVATE src)
    add_test(NAME fog_math COMMAND fog_math_test)
    add_executable(draw_semantics_test tests/draw_semantics_test.cpp src/draw_semantics.cpp src/draw_segments.cpp)
    target_compile_features(draw_semantics_test PRIVATE cxx_std_20)
    target_include_directories(draw_semantics_test PRIVATE src)
    add_test(NAME draw_semantics COMMAND draw_semantics_test)
    add_executable(game_lights_test tests/game_lights_test.cpp src/game_lights.cpp)
    target_compile_features(game_lights_test PRIVATE cxx_std_20)
    target_include_directories(game_lights_test PRIVATE src)
    add_test(NAME game_lights COMMAND game_lights_test)
    add_executable(game_fog_test tests/game_fog_test.cpp src/game_fog.cpp)
    target_compile_features(game_fog_test PRIVATE cxx_std_20)
    target_include_directories(game_fog_test PRIVATE src)
    add_test(NAME game_fog COMMAND game_fog_test)
    add_executable(game_pcf_test tests/game_pcf_test.cpp src/game_pcf.cpp)
    target_compile_features(game_pcf_test PRIVATE cxx_std_20)
    target_include_directories(game_pcf_test PRIVATE src)
    add_test(NAME game_pcf COMMAND game_pcf_test)
    add_executable(spirv_soft_shadows_test tests/spirv_soft_shadows_test.cpp src/spirv_soft_shadows.cpp)
    target_compile_features(spirv_soft_shadows_test PRIVATE cxx_std_20)
    target_include_directories(spirv_soft_shadows_test PRIVATE src)
    target_compile_definitions(spirv_soft_shadows_test PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    find_library(DEPTH_PROBE_SPIRV_TOOLS NAMES SPIRV-Tools-shared)   # optional: the patched shaders also pass the SPIR-V validator
    if(DEPTH_PROBE_SPIRV_TOOLS)
        target_link_libraries(spirv_soft_shadows_test PRIVATE ${DEPTH_PROBE_SPIRV_TOOLS})
        target_compile_definitions(spirv_soft_shadows_test PRIVATE DEPTH_PROBE_HAVE_SPIRV_TOOLS=1)
    endif()
    add_test(NAME spirv_soft_shadows COMMAND spirv_soft_shadows_test)
    add_executable(fog_vulkan_smoke tests/fog_vulkan_smoke.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(fog_vulkan_smoke PRIVATE cxx_std_20)
    target_include_directories(fog_vulkan_smoke PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(fog_vulkan_smoke PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_definitions(fog_vulkan_smoke PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(soft_shadows_vulkan_smoke tests/soft_shadows_vulkan_smoke.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(soft_shadows_vulkan_smoke PRIVATE cxx_std_20)
    target_include_directories(soft_shadows_vulkan_smoke PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(soft_shadows_vulkan_smoke PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_definitions(soft_shadows_vulkan_smoke PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(water_vulkan_smoke tests/water_vulkan_smoke.cpp ${DEPTH_PROBE_LAYER_SOURCES})
    target_compile_features(water_vulkan_smoke PRIVATE cxx_std_20)
    target_include_directories(water_vulkan_smoke PRIVATE src "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(water_vulkan_smoke PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(water_vulkan_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(water_vulkan_smoke PRIVATE DEPTH_PROBE_TEST_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(layer_lifecycle PROPERTIES
        ENVIRONMENT "DEPTH_PROBE_SHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders;DEPTH_PROBE_UI_INPUT=0")
endif()
