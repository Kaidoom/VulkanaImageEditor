foreach(component IN ITEMS CloneReference CloneStroke Healing SpotHealRepair SpotHealStroke BoundedParallel)
    string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" test_name "${component}")
    string(TOLOWER "${test_name}" test_name)
    set(target "imageeditor_${test_name}_tests")
    add_executable(${target} "${component}Tests.cpp")
    target_link_libraries(${target} PRIVATE imageeditor_core)
    imageeditor_enable_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 90 LABELS "cloning;core;deterministic")
endforeach()
# The full transformed/effected-reference matrix runs real reconstruction in
# unoptimized Debug too; keep the deterministic work budget, not a Release-only
# wall-clock allowance inherited from the short Stamp/Heal tests.
set_tests_properties(imageeditor_spot_heal_stroke_tests PROPERTIES TIMEOUT 240)

foreach(component IN ITEMS Options Interaction)
    string(TOLOWER "${component}" name)
    set(target "imageeditor_cloning_${name}_tests")
    add_executable(${target} "Cloning${component}Tests.cpp")
    target_link_libraries(${target} PRIVATE imageeditor_ui Qt6::Test)
    imageeditor_enable_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 90 LABELS "cloning;input;ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
add_test(NAME imageeditor_spot_heal_ui_tests COMMAND imageeditor_cloning_interaction_tests --spot-heal)
set_tests_properties(imageeditor_spot_heal_ui_tests PROPERTIES TIMEOUT 120
    LABELS "cloning;spot-heal;ui;history;export" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_cloning_smoke COMMAND imageeditor_cloning_interaction_tests --native)
    set_tests_properties(imageeditor_wayland_cloning_smoke PROPERTIES
        TIMEOUT 90 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS "cloning;integration;wayland;vulkan"
        ENVIRONMENT "QT_QPA_PLATFORM=wayland")
    add_test(NAME imageeditor_wayland_spot_heal COMMAND imageeditor_cloning_interaction_tests --native --spot-heal)
    set_tests_properties(imageeditor_wayland_spot_heal PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77
        LABELS "cloning;spot-heal;integration;wayland;vulkan" ENVIRONMENT "QT_QPA_PLATFORM=wayland")
endif()

add_executable(imageeditor_cloning_comparison CloningComparison.cpp)
target_link_libraries(imageeditor_cloning_comparison PRIVATE imageeditor_core Qt6::Gui)
imageeditor_enable_warnings(imageeditor_cloning_comparison)

add_executable(imageeditor_spot_heal_benchmark SpotHealBenchmark.cpp)
target_link_libraries(imageeditor_spot_heal_benchmark PRIVATE imageeditor_core Qt6::Gui Qt6::Core)
imageeditor_enable_warnings(imageeditor_spot_heal_benchmark)

add_executable(imageeditor_spot_heal_equivalence_tests SpotHealEquivalenceTests.cpp
    fixtures/SpotHealRepairV1.cpp)
set_source_files_properties(fixtures/SpotHealRepairV1.cpp PROPERTIES
    COMPILE_DEFINITIONS "repairSpotHeal=repairSpotHealFrozenReference")
target_link_libraries(imageeditor_spot_heal_equivalence_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_spot_heal_equivalence_tests)
add_test(NAME imageeditor_spot_heal_equivalence_tests COMMAND imageeditor_spot_heal_equivalence_tests)
set_tests_properties(imageeditor_spot_heal_equivalence_tests PROPERTIES
    TIMEOUT 120 LABELS "cloning;spot-heal;core;deterministic;parallel")

add_subdirectory("${PROJECT_SOURCE_DIR}/tools/spot-heal-gpu-probe"
    "${PROJECT_BINARY_DIR}/tools/spot-heal-gpu-probe")
