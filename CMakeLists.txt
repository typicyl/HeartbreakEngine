# Heartbreak Engine targets
#
# Two engine libraries are built from the same core sources:
#   * hbe         - the runtime (NO editor, NO Dear ImGui). Ships with the game.
#   * hbe_editor  - runtime + editor (ImGui/ImGuizmo/asset browser). Tools only.
# Editor-only code is gated behind HBE_EDITOR so the runtime binary contains none
# of it. Two executables: HeartbreakRuntime (game) and HeartbreakEditor (tool).

# ---------------------------------------------------------------------------
# Source groups
# ---------------------------------------------------------------------------
set(HBE_CORE_SOURCES
    "Source/Core/Log.cpp"
    "Source/Core/JobSystem.cpp"
    "Source/Core/Window_Win32.cpp"
    "Source/Core/Input_Win32.cpp")

set(HBE_ASSET_SOURCES
    "Source/Assets/MeshGenerator.cpp"
    "Source/Assets/StrokeGen.cpp"
    "Source/Assets/ModelLoader.cpp"
    "Source/Assets/UAF.cpp"
    "Source/Assets/UAP.cpp"
    "Source/Assets/VFS.cpp"
    "Source/Assets/MaterialAsset.cpp"
    "Source/Assets/AudioEvent.cpp"
    "Source/Assets/AssetLoader.cpp")

set(HBE_SCENE_SOURCES
    "Source/Scene/Scene.cpp"
    "Source/Scene/AnimationSystem.cpp"
    "Source/Scene/CameraSystem.cpp"
    "Source/Scene/CharacterController.cpp"
    "Source/Scene/TerrainSystem.cpp"
    "Source/Scene/PaintSystem.cpp"
    "Source/Scene/ParticleSystem.cpp"
    "Source/Scene/Level.cpp"
    "Source/Scene/SceneSerializer.cpp"
    "Source/Scene/SceneStreamer.cpp"
    "Source/Scene/StreamingWorld.cpp")

set(HBE_PHYSICS_SOURCES
    "Source/Physics/PhysicsWorld.cpp")

set(HBE_NAV_SOURCES
    "Source/Navigation/GridNav.cpp")

set(HBE_GAME_SOURCES
    "Source/Game/GameSystems.cpp")

set(HBE_AUDIO_SOURCES
    "Source/Audio/AudioSystem.cpp")

set(HBE_PROJECT_SOURCES
    "Source/Project/Project.cpp")

set(HBE_SCHEMATIC_SOURCES
    "Source/Schematic/Schematic.cpp"
    "Source/Schematic/SchematicSystem.cpp"
    "Source/Schematic/SchematicTranspile.cpp")

set(HBE_UI_SOURCES
    "Source/UI/UISystem.cpp"
    "Source/UI/FontAtlas.cpp")

set(HBE_RHI_SOURCES
    "Source/RHI/RHIFactory.cpp")

set(HBE_RENDERER_SOURCES
    "Source/Renderer/Renderer.cpp"
    "Source/Renderer/CameraController.cpp"
    "Source/Renderer/IBL.cpp")

set(HBE_ENGINE_SOURCES
    "Source/Engine/Engine.cpp")

set(HBE_D3D12_SOURCES  "Source/RHI/D3D12/D3D12Device.cpp")
set(HBE_VULKAN_SOURCES "Source/RHI/Vulkan/VulkanDevice.cpp")
set(HBE_GL_SOURCES     "Source/RHI/GL/GLDevice.cpp"
                       "Source/RHI/GL/GLApi.cpp")

# Editor-only sources (added only to hbe_editor).
set(HBE_EDITOR_SOURCES
    "Source/Editor/Editor.cpp"
    "Source/Editor/Importer.cpp"
    "Source/Editor/MeshThumbnail.cpp"
    "Source/Editor/stb_image_impl.cpp")

# Core sources common to both the runtime and editor libraries.
set(HBE_RUNTIME_SOURCES
    ${HBE_CORE_SOURCES}
    ${HBE_ASSET_SOURCES}
    ${HBE_SCENE_SOURCES}
    ${HBE_PHYSICS_SOURCES}
    ${HBE_NAV_SOURCES}
    ${HBE_GAME_SOURCES}
    ${HBE_AUDIO_SOURCES}
    ${HBE_PROJECT_SOURCES}
    ${HBE_SCHEMATIC_SOURCES}
    ${HBE_UI_SOURCES}
    ${HBE_RHI_SOURCES}
    ${HBE_RENDERER_SOURCES}
    ${HBE_ENGINE_SOURCES})

# ---------------------------------------------------------------------------
# Shared engine-library configuration
# ---------------------------------------------------------------------------
function(hbe_configure_engine_lib tgt is_editor)
    target_include_directories(${tgt} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_features(${tgt} PUBLIC cxx_std_20)

    target_link_libraries(${tgt} PUBLIC glm::glm EnTT::EnTT)
    target_link_libraries(${tgt} PRIVATE assimp::assimp nlohmann_json::nlohmann_json)
    # XInput (gamepad). xinput9_1_0 ships with Windows itself (no redist).
    target_link_libraries(${tgt} PRIVATE xinput9_1_0)
    # Windows Compression API (UAP pack compression). Ships with Windows 8+.
    target_link_libraries(${tgt} PRIVATE cabinet)
    # stb single-header utilities: stb_easy_font for the in-game UI (runtime
    # too); the editor additionally uses stb_image for texture import.
    target_link_libraries(${tgt} PRIVATE stb)
    # Jolt rigid-body physics (Source/Physics wraps it; no Jolt in headers).
    target_link_libraries(${tgt} PRIVATE Jolt)
    # Navigation is a custom grid A* pathfinder (Source/Navigation/GridNav) - no
    # external nav dependency.
    # miniaudio playback (Source/Audio wraps it; header-only).
    target_link_libraries(${tgt} PRIVATE miniaudio_header)
    target_compile_definitions(${tgt} PUBLIC
        GLM_FORCE_DEPTH_ZERO_TO_ONE  # match D3D12/Vulkan [0,1] clip-space depth
        GLM_FORCE_RADIANS
        GLM_ENABLE_EXPERIMENTAL
        HBE_EDITOR=${is_editor})

    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive- /MP)
        target_compile_definitions(${tgt} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()

    if(HBE_ENABLE_D3D12)
        target_sources(${tgt} PRIVATE ${HBE_D3D12_SOURCES})
        target_compile_definitions(${tgt} PUBLIC HBE_ENABLE_D3D12=1)
        target_link_libraries(${tgt} PRIVATE d3d12 dxgi dxguid)
    else()
        target_compile_definitions(${tgt} PUBLIC HBE_ENABLE_D3D12=0)
    endif()

    if(HBE_ENABLE_VULKAN)
        find_package(Vulkan REQUIRED)
        target_sources(${tgt} PRIVATE ${HBE_VULKAN_SOURCES})
        target_compile_definitions(${tgt} PUBLIC HBE_ENABLE_VULKAN=1)
        target_compile_definitions(${tgt} PRIVATE VK_USE_PLATFORM_WIN32_KHR)
        target_link_libraries(${tgt} PUBLIC Vulkan::Vulkan)
    else()
        target_compile_definitions(${tgt} PUBLIC HBE_ENABLE_VULKAN=0)
    endif()

    if(HBE_ENABLE_OPENGL)
        target_sources(${tgt} PRIVATE ${HBE_GL_SOURCES})
        target_compile_definitions(${tgt} PUBLIC HBE_ENABLE_OPENGL=1)
        target_link_libraries(${tgt} PRIVATE opengl32)
    else()
        target_compile_definitions(${tgt} PUBLIC HBE_ENABLE_OPENGL=0)
    endif()

    # Only the editor library pulls in Dear ImGui + ImGuizmo.
    if(is_editor)
        target_link_libraries(${tgt} PUBLIC imgui)
        # comdlg32 = file dialogs, ole32 = IFileDialog folder picker (COM).
        target_link_libraries(${tgt} PRIVATE comdlg32 ole32)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------
add_library(hbe STATIC ${HBE_RUNTIME_SOURCES})
add_library(HBE::Runtime ALIAS hbe)
hbe_configure_engine_lib(hbe 0)

add_library(hbe_editor STATIC ${HBE_RUNTIME_SOURCES} ${HBE_EDITOR_SOURCES})
add_library(HBE::Editor ALIAS hbe_editor)
hbe_configure_engine_lib(hbe_editor 1)
# Editor.cpp + SceneSerializer.cpp have grown past the COFF per-object section
# limit (lots of template instantiations); /bigobj lifts it.
if(MSVC)
    set_source_files_properties(
        "Source/Editor/Editor.cpp" "Source/Scene/SceneSerializer.cpp"
        PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()

# ---------------------------------------------------------------------------
# Schematic baking: graphs -> native C++ compiled into the runtime
# ---------------------------------------------------------------------------
# HeartbreakBaker transpiles a project's .hbschem files into one C++ translation
# unit (RegisterBakedSchematics) that the runtime links so graphs run as machine
# code, not through the interpreter.
add_executable(HeartbreakBaker "Source/Tools/SchematicBaker.cpp")
target_link_libraries(HeartbreakBaker PRIVATE hbe)
set_target_properties(HeartbreakBaker PROPERTIES VS_DPI_AWARE "PerMonitor")

# Every executable links a TU defining RegisterBakedSchematics(). The editor/hub/
# art tools (and an un-baked runtime) use the stub = interpreter. Set
# HBE_GAME_PROJECT to a project root (folder with Assets/) to bake that project's
# schematics into HeartbreakRuntime instead.
set(HBE_SCHEMATIC_STUB "Source/Schematic/SchematicBakedStub.cpp")
set(HBE_GAME_PROJECT "" CACHE PATH
    "Project root whose .hbschem graphs are baked into HeartbreakRuntime as native C++ (empty = interpreter).")
if(HBE_GAME_PROJECT)
    set(HBE_RUNTIME_BAKED "${CMAKE_CURRENT_BINARY_DIR}/SchematicBaked.gen.cpp")
    add_custom_command(
        OUTPUT "${HBE_RUNTIME_BAKED}"
        COMMAND HeartbreakBaker "${HBE_GAME_PROJECT}" "${HBE_RUNTIME_BAKED}"
        DEPENDS HeartbreakBaker
        COMMENT "Baking schematics from ${HBE_GAME_PROJECT} into native C++"
        VERBATIM)
    message(STATUS "Schematics: baking from '${HBE_GAME_PROJECT}' into HeartbreakRuntime.")
else()
    set(HBE_RUNTIME_BAKED "${HBE_SCHEMATIC_STUB}")
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(HeartbreakRuntime "Source/main_runtime.cpp" "${HBE_RUNTIME_BAKED}")
target_link_libraries(HeartbreakRuntime PRIVATE hbe)
set_target_properties(HeartbreakRuntime PROPERTIES VS_DPI_AWARE "PerMonitor")

add_executable(HeartbreakEditor "Source/main_editor.cpp" "${HBE_SCHEMATIC_STUB}")
target_link_libraries(HeartbreakEditor PRIVATE hbe_editor)
set_target_properties(HeartbreakEditor PROPERTIES VS_DPI_AWARE "PerMonitor")

# The Hub: standalone project manager that launches the editor.
add_executable(HeartbreakHub "Source/main_hub.cpp" "${HBE_SCHEMATIC_STUB}")
target_link_libraries(HeartbreakHub PRIVATE hbe_editor)
set_target_properties(HeartbreakHub PROPERTIES VS_DPI_AWARE "PerMonitor")

# The Art Editor: a focused painting tool for 2D artists (same editor lib).
add_executable(HeartbreakArtEditor "Source/main_arteditor.cpp" "${HBE_SCHEMATIC_STUB}")
target_link_libraries(HeartbreakArtEditor PRIVATE hbe_editor)
set_target_properties(HeartbreakArtEditor PROPERTIES VS_DPI_AWARE "PerMonitor")

# Compile HLSL -> DXIL + SPIR-V and stage next to each executable.
include(ShaderCompile)
hbe_compile_engine_shaders(HeartbreakEditor)
hbe_compile_engine_shaders(HeartbreakRuntime)
hbe_compile_engine_shaders(HeartbreakHub)
hbe_compile_engine_shaders(HeartbreakArtEditor)

