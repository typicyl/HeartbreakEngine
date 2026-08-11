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
    "Source/Core/UserSettings.cpp"
    "Source/Core/InputActions.cpp"
    # PLATFORM LAYER. Selected by OS exactly the way the RHI backends are selected by
    # graphics API - a second platform adds Platform_Linux.cpp beside this and changes
    # nothing else. The conditional is worth writing now, with one backend, because it is
    # what makes the file's role obvious to the next reader.
    $<$<BOOL:${WIN32}>:Source/Core/Platform_Win32.cpp>
    # The process crash handler is a SEPARATE backend TU from Platform_Win32.cpp on purpose:
    # it depends on Core/Log, whereas Platform_Win32.cpp is ALSO linked (explicitly, below)
    # into the engine-free hbe_hubcore, which has no Log.cpp. Living in HBE_CORE_SOURCES - which
    # only the engine libraries pull in, never hbe_hubcore - keeps its Log dependency out of the
    # Hub's link. A second platform adds CrashHandler_Posix.cpp beside it.
    $<$<BOOL:${WIN32}>:Source/Core/CrashHandler_Win32.cpp>
    "Source/Core/Window_Win32.cpp"
    # Input is SPLIT: the frame logic, press edges, text stream and deadzone curve are
    # portable and unconditional; only key translation and the gamepad poll are per-OS.
    "Source/Core/Input.cpp"
    $<$<BOOL:${WIN32}>:Source/Core/Input_Win32.cpp>)

set(HBE_ASSET_SOURCES
    "Source/Assets/AssetFormats.cpp"
    "Source/Assets/AssetRefs.cpp"
    "Source/Assets/AssetRefsTest.cpp"
    "Source/Assets/SlotIds.cpp"
    "Source/Assets/SlotIdsTest.cpp"
    "Source/Assets/Fracture.cpp"
    "Source/Assets/MeshGenerator.cpp"
    "Source/Assets/StrokeGen.cpp"
    "Source/Assets/ModelLoader.cpp"
    "Source/Assets/UAF.cpp"
    "Source/Assets/UAP.cpp"
    "Source/Assets/VFS.cpp"
    "Source/Assets/MaterialAsset.cpp"
    "Source/Assets/CharacterAsset.cpp"
    "Source/Assets/SeamWeld.cpp"
    "Source/Assets/CharacterBuild.cpp"
    "Source/Assets/AudioEvent.cpp"
    "Source/Assets/MusicGraph.cpp"
    "Source/Assets/DialogueAsset.cpp"
    "Source/Assets/CutsceneAsset.cpp"
    "Source/Assets/AssetLoader.cpp")

set(HBE_SCENE_SOURCES
    "Source/Scene/Scene.cpp"
    "Source/Scene/EntityGuid.cpp"
    "Source/Scene/AnimationSystem.cpp"
    "Source/Scene/BodyShape.cpp"
    "Source/Assets/MeshDerive.cpp"
    "Source/Assets/MeshSimplify.cpp"
    "Source/Assets/MeshFaceSelect.cpp"
    "Source/Scene/CameraRig.cpp"
    "Source/Scene/CameraSystem.cpp"
    "Source/Scene/CharacterController.cpp"
    "Source/Scene/CharacterSystem.cpp"
    "Source/Scene/FacialSystem.cpp"
    "Source/Scene/Hierarchy.cpp"
    "Source/Scene/TerrainSystem.cpp"
    "Source/Scene/PaintSystem.cpp"
    "Source/Scene/ParticleSystem.cpp"
    "Source/Scene/WeatherSystem.cpp"
    "Source/Scene/PrecipSystem.cpp"
    "Source/Scene/DecalSystem.cpp"
    "Source/Scene/WaterSystem.cpp"
    "Source/Scene/OceanFFT.cpp"
    "Source/Scene/LightningSystem.cpp"
    "Source/Scene/ParticleGpuSim.cpp"
    "Source/Scene/SceneSerializer.cpp"
    "Source/Scene/SceneStreamer.cpp"
    "Source/Scene/StreamPolicy.cpp"
    "Source/Scene/StrokeZone.cpp"
    "Source/Scene/TagShard.cpp"
    "Source/Scene/TagStreaming.cpp"
    "Source/Scene/TagTable.cpp"
    "Source/Scene/SceneJournal.cpp"
    "Source/Scene/WorldLocalTest.cpp"
    "Source/Dialogue/GraphFanInTest.cpp"
    "Source/Scene/WorldState.cpp")

set(HBE_PHYSICS_SOURCES
    "Source/Physics/PhysicsWorld.cpp")

set(HBE_NAV_SOURCES
    "Source/Navigation/GridNav.cpp")

# The unified pick pass (world-UI pages + 3D interactables, one ray, one winner).
set(HBE_INTERACT_SOURCES
    "Source/Interaction/Pick.cpp"
    "Source/Interaction/Interact3DTest.cpp")

set(HBE_GAME_SOURCES
    "Source/Game/GameSystems.cpp"
    "Source/Game/CombatSystem.cpp"
    "Source/Game/AISystem.cpp"
    "Source/Game/SpawnSystem.cpp"
    "Source/Game/DestructionSystem.cpp"
    "Source/Game/InventorySystem.cpp"
    "Source/Game/GameplaySystems.cpp")

# Module-stack VFX core (attribute model, compiler/validator, CPU kernels).
# Runtime-side on purpose: the editor only authors these stacks, it does not own them.
set(HBE_VFX_SOURCES
    "Source/Vfx/VfxStack.cpp"
    "Source/Vfx/VfxLegacy.cpp")

set(HBE_AUDIO_SOURCES
    "Source/Audio/AudioSystem.cpp")

set(HBE_PROJECT_SOURCES
    "Source/Project/Project.cpp")

set(HBE_SCHEMATIC_SOURCES
    "Source/Schematic/Schematic.cpp"
    "Source/Schematic/SchematicSystem.cpp"
    "Source/Schematic/SchematicTranspile.cpp")

set(HBE_DIALOGUE_SOURCES
    "Source/Dialogue/DialogueGraph.cpp")

set(HBE_UI_SOURCES
    "Source/UI/Subtitles.cpp"
    "Source/UI/UISystem.cpp"
    "Source/UI/UIAnimation.cpp"
    "Source/UI/UIDocument.cpp"
    "Source/UI/UIManager.cpp"
    "Source/UI/UIWorld.cpp"
    "Source/UI/UIFocus.cpp"
    "Source/UI/FontAtlas.cpp")

set(HBE_RHI_SOURCES
    "Source/RHI/RHIFactory.cpp")

set(HBE_RENDERER_SOURCES
    "Source/Renderer/Renderer.cpp"
    "Source/Renderer/CameraController.cpp"
    "Source/Renderer/IBL.cpp")

set(HBE_ENGINE_SOURCES
    "Source/Engine/CutscenePlayer.cpp"
    "Source/Engine/Engine.cpp")

set(HBE_D3D12_SOURCES  "Source/RHI/D3D12/D3D12Device.cpp")
set(HBE_VULKAN_SOURCES "Source/RHI/Vulkan/VulkanDevice.cpp"
    # The Win32 VkSurfaceKHR backend - the ONLY Vulkan TU that includes <windows.h> and defines
    # VK_USE_PLATFORM_WIN32_KHR (locally). Selected by OS like every other backend; a port adds
    # VulkanSurface_Linux.cpp beside it. See Source/RHI/Vulkan/VulkanSurface.h.
    $<$<BOOL:${WIN32}>:Source/RHI/Vulkan/VulkanSurface_Win32.cpp>)
set(HBE_GL_SOURCES     "Source/RHI/GL/GLDevice.cpp"
                       "Source/RHI/GL/GLApi.cpp")

# Collaboration layer. Depends on Core/ ONLY - no scene, no renderer, no editor, no
# glm - so the same objects link into a headless server with no GPU and no shader
# step. Enforced by inspection:
#   grep -rE '#include "(Scene|Renderer|RHI|Editor|Engine|UI|Assets|Game)/' Source/Collab/
# must stay empty.
set(HBE_COLLAB_SOURCES
    "Source/Collab/Protocol.cpp"
    "Source/Collab/LoopbackTransport.cpp"
    "Source/Collab/LockTable.cpp"
    "Source/Collab/CollabServer.cpp"
    "Source/Collab/CollabClient.cpp"
    "Source/Collab/Journal.cpp"
    "Source/Collab/JournalSelfTest.cpp"
    "Source/Collab/TcpTransport.cpp"
    "Source/Collab/Identity.cpp"
    "Source/Collab/SecureChannel.cpp"
    "Source/Collab/ProjectSync.cpp"
    "Source/Collab/ProjectFetch.cpp"
    "Source/Collab/CollabSelfTest.cpp")

# Peer-to-peer transport. EDITOR ONLY - see hbe_configure_engine_lib. Kept out of
# HBE_COLLAB_SOURCES on purpose so the shipped runtime never links libdatachannel.
set(HBE_COLLAB_P2P_SOURCES
    "Source/Collab/Signaling.cpp"
    "Source/Collab/WebRtcTransport.cpp"
    "Source/Editor/CollabSession.cpp"
    "Source/Editor/CollabPanels.cpp")

# Hub / launcher: engine version, update manifest, HTTPS fetch, safe zip extraction
# and the staged-swap installer. Core/ + Windows only - the Hub must be able to run in
# order to REPAIR a broken engine install, so it must not need one to start.
# THE HUB IS WHERE SOMEONE GETS EVERYTHING, so it must be able to JOIN a session and
# pull a project down BEFORE any engine or project exists. Requiring them to install the
# engine, invent an empty project and open it just to reach the download defeats the
# point of having a launcher at all.
#
# This is only possible because Source/Collab/ was kept free of engine includes: the
# client half - identity, the secure channel, ICE, the protocol, the file transfer -
# links here with no scene, no renderer and no entt. The SERVER half and the journal
# stay out; the Hub joins, it does not host.
set(HBE_HUB_COLLAB_SOURCES
    "Source/Collab/Protocol.cpp"
    "Source/Collab/Identity.cpp"
    "Source/Collab/SecureChannel.cpp"
    "Source/Collab/Signaling.cpp"
    "Source/Collab/CollabClient.cpp"
    "Source/Collab/WebRtcTransport.cpp"
    "Source/Collab/ProjectSync.cpp"
    "Source/Collab/ProjectFetch.cpp"
    "Source/Hub/HubJoin.cpp")

set(HBE_HUB_SOURCES
    "Source/Hub/UpdateCheck.cpp"
    "Source/Hub/ZipArchive.cpp"
    "Source/Hub/Updater.cpp"
    "Source/Hub/HubSelfUpdate.cpp"
    "Source/Hub/HubConfig.cpp"
    "Source/Hub/ProjectCatalog.cpp"
    "Source/Hub/HubSelfTest.cpp"
    ${HBE_HUB_COLLAB_SOURCES})

# Editor-only sources (added only to hbe_editor).
set(HBE_EDITOR_SOURCES
    # Native GUI services (file/folder pickers, desktop colour sampling) - the editor-only
    # slice of the platform layer, selected by OS the same way the RHI backends are. It drags
    # in COM/comdlg/GDI, which is exactly why it is here and not in the runtime's Platform
    # backend: a shipped game exe never links it. A second platform adds NativeDialogs_*.cpp.
    $<$<BOOL:${WIN32}>:Source/Core/NativeDialogs_Win32.cpp>
    "Source/Editor/Editor.cpp"
    "Source/Editor/SaveDispatch.cpp"
    "Source/Editor/TimelineSnap.cpp"
    "Source/Editor/SceneSaveTest.cpp"
    "Source/Editor/EditorZoneTest.cpp"
    "Source/Editor/PasteParentTest.cpp"
    "Source/Editor/DialogueEditor.cpp"
    "Source/Editor/UIEditor.cpp"
    "Source/Editor/Importer.cpp"
    "Source/Editor/MeshThumbnail.cpp"
    "Source/Editor/MovieRender.cpp"
    "Source/Editor/MovieEncoder.cpp"
    "Source/Editor/stb_image_impl.cpp"
    "Source/Editor/stb_image_write_impl.cpp")

# NanoVDB volumetric foundation (baker / asset / cache / runtime). Header-only NanoVDB,
# zero external runtime deps. OpenVDB (editor-only, vcpkg) is a later optional addition.
set(HBE_VOLUME_SOURCES
    "Source/Volume/VolumeNano.cpp")

# Core sources common to both the runtime and editor libraries.
set(HBE_RUNTIME_SOURCES
    ${HBE_CORE_SOURCES}
    ${HBE_VOLUME_SOURCES}
    ${HBE_ASSET_SOURCES}
    ${HBE_SCENE_SOURCES}
    ${HBE_PHYSICS_SOURCES}
    ${HBE_NAV_SOURCES}
    ${HBE_INTERACT_SOURCES}
    ${HBE_GAME_SOURCES}
    ${HBE_VFX_SOURCES}
    ${HBE_AUDIO_SOURCES}
    ${HBE_PROJECT_SOURCES}
    ${HBE_SCHEMATIC_SOURCES}
    ${HBE_DIALOGUE_SOURCES}
    ${HBE_UI_SOURCES}
    ${HBE_COLLAB_SOURCES}
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
    # shell32 = DragAcceptFiles / WM_DROPFILES (OS file drops onto the window).
    target_link_libraries(${tgt} PRIVATE shell32)
    # Windows Compression API (UAP pack compression). Ships with Windows 8+.
    target_link_libraries(${tgt} PRIVATE cabinet)
    # WinSock2 (the collaboration TCP transport). Ships with Windows; no redist, and
    # the same kind of in-box import lib as xinput9_1_0/cabinet above - so the
    # collaboration layer adds no third-party dependency.
    # THE ENGINE VERSION, and the only place it is defined.
    #
    # It must be an ORDERED triple that reaches C++, because the updater's entire job is
    # answering "is the remote newer than me?". The boot log's __DATE__/__TIME__ stamp
    # cannot answer that - it does not sort - and the outer project(VERSION) never
    # reached a single translation unit. Bump these to publish a release.
    target_compile_definitions(${tgt} PRIVATE
        HBE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        HBE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        HBE_VERSION_PATCH=${PROJECT_VERSION_PATCH})
    target_link_libraries(${tgt} PRIVATE ws2_32)
    # Mbed TLS: transport security for collaboration over the open internet, and BCrypt
    # for the per-install identity keypair. See Source/Collab/SecureChannel.h for why a
    # library is the right call for the handshake specifically.
    target_link_libraries(${tgt} PRIVATE mbedtls mbedx509 mbedcrypto bcrypt)
    # The launcher core, for --test-hub. One-way: hbe_hubcore never links the engine.
    target_link_libraries(${tgt} PRIVATE hbe_hubcore)
    # zlib (raw inflate) for the Hub's release-archive reader. assimp already builds
    # zlibstatic, so this adds no new download - only a link and an include path.
    target_link_libraries(${tgt} PRIVATE zlibstatic)
    target_include_directories(${tgt} PRIVATE
        "${assimp_SOURCE_DIR}/contrib/zlib"
        "${assimp_BINARY_DIR}/contrib/zlib")
    # stb single-header utilities: stb_easy_font for the in-game UI (runtime
    # too); the editor additionally uses stb_image for texture import.
    target_link_libraries(${tgt} PRIVATE stb)
    # Jolt rigid-body physics (Source/Physics wraps it; no Jolt in headers).
    target_link_libraries(${tgt} PRIVATE Jolt)
    # Navigation is a custom grid A* pathfinder (Source/Navigation/GridNav) - no
    # external nav dependency.
    # miniaudio playback (Source/Audio wraps it; header-only).
    target_link_libraries(${tgt} PRIVATE miniaudio_header)
    # NanoVDB - header-only sparse-volume representation + PNanoVDB GPU reader. The
    # volumetric foundation (Source/Volume wraps it; no NanoVDB types cross into the RHI).
    # Zero external deps here; OpenVDB stays an optional editor-only add-on (vcpkg) later.
    target_link_libraries(${tgt} PRIVATE nanovdb)
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
        # VK_USE_PLATFORM_WIN32_KHR is deliberately NOT defined engine-wide anymore: it pulls
        # vulkan_win32.h (and thus a need for <windows.h>) into every Vulkan TU. It now lives
        # ONLY in VulkanSurface_Win32.cpp, so the rest of the Vulkan backend - and the whole
        # shipped runtime - sees core vulkan.h with no Win32 dependency.
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
        # WebRTC (peer-to-peer collaboration) is an AUTHORING feature. Linking it into
        # `hbe` would put an ICE/DTLS/SCTP stack, and its threads, into every shipped
        # game that will never open a collaboration session.
        target_sources(${tgt} PRIVATE ${HBE_COLLAB_P2P_SOURCES})
        target_link_libraries(${tgt} PRIVATE LibDataChannel::LibDataChannelStatic)
        # comdlg32 = file dialogs, ole32 = IFileDialog folder picker (COM).
        # mf* = Media Foundation H.264 .mp4 encode/decode (offline movie render).
        target_link_libraries(${tgt} PRIVATE comdlg32 ole32 mfplat mfreadwrite mfuuid mf)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------
# The fiber job system migrates fibers between worker threads, which read
# thread_local state (t_scheduler / t_active) across SwitchToFiber. Without /GT
# the optimizer caches the TLS slot address in a register; after a fiber resumes
# on a DIFFERENT thread that cached address is stale, so it switches into a fiber
# already running elsewhere -> stack corruption (access violation that even
# defeats SEH). /GT = MSVC fiber-safe TLS: re-fetch TLS per access. Debug doesn't
# cache TLS, which is why only optimized builds crashed.
if(MSVC)
    set_source_files_properties("Source/Core/JobSystem.cpp"
                                PROPERTIES COMPILE_OPTIONS "/GT")
endif()

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

# NOTE: the MSVC runtime is linked STATICALLY (/MT - see the top-level CMakeLists), so
# the executables need no msvcp140/vcruntime140/concrt140 DLLs and nothing is staged
# next to them. A shipped build is just the exe + the .uap packs.

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
# ---------------------------------------------------------------------------
# hbe_hubcore - the launcher's logic, with NO ENGINE DEPENDENCY.
#
# Deliberately its own library so HeartbreakHub can link this and nothing else. The
# Hub must run when the engine is broken, half-installed or absent - that is exactly
# when a person opens a launcher - and it must live OUTSIDE the bin/ directory it
# swaps during an update, because a process cannot rename the directory it runs from.
# The engine libs link it too (for --test-hub), which is a one-way dependency.
# ---------------------------------------------------------------------------
# The platform layer is shared with the engine. It pulls in nothing but <windows.h> and
# Core/Types.h, so it does not violate the Hub's "links no engine code" rule - and THREE of
# the six duplicated %LOCALAPPDATA% lookups lived in Hub/ , which is precisely the
# duplication this layer exists to delete.
add_library(hbe_hubcore STATIC ${HBE_HUB_SOURCES}
    $<$<BOOL:${WIN32}>:Source/Core/Platform_Win32.cpp>)
target_include_directories(hbe_hubcore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_compile_features(hbe_hubcore PUBLIC cxx_std_20)
target_link_libraries(hbe_hubcore PRIVATE nlohmann_json::nlohmann_json zlibstatic)
target_include_directories(hbe_hubcore PRIVATE
    "${assimp_SOURCE_DIR}/contrib/zlib" "${assimp_BINARY_DIR}/contrib/zlib")
# All in-box Windows libs: WinHTTP (https), BCrypt (SHA-256). No third-party runtime.
target_compile_definitions(hbe_hubcore PRIVATE HBE_COLLAB_JOIN_ONLY=1)
target_link_libraries(hbe_hubcore PRIVATE winhttp bcrypt ws2_32)
# The same peer-to-peer stack the editor uses - see HBE_HUB_COLLAB_SOURCES.
target_link_libraries(hbe_hubcore PRIVATE mbedtls mbedx509 mbedcrypto)
target_link_libraries(hbe_hubcore PRIVATE LibDataChannel::LibDataChannelStatic)
# The Hub SHIPS INSIDE the engine payload, so its version is the engine's version by
# definition - taking it from anywhere else is how the two drift apart.
target_compile_definitions(hbe_hubcore PUBLIC
    HBE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    HBE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    HBE_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# imgui_hub - the SAME Dear ImGui sources, but only the Win32 + D3D11 backends. The
# engine's `imgui` target bundles the D3D12 and Vulkan backends, and linking it would
# drag vulkan-1.dll back into the launcher - the exact import that made the old hub
# fail to start on a machine with no GPU driver.
add_library(imgui_hub STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(imgui_hub PUBLIC
    "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_features(imgui_hub PUBLIC cxx_std_20)

add_executable(HeartbreakHub WIN32 "Source/HubApp/main_hubapp.cpp")
target_link_libraries(HeartbreakHub PRIVATE hbe_hubcore imgui_hub d3d11 dxgi)
set_target_properties(HeartbreakHub PROPERTIES VS_DPI_AWARE "PerMonitor")

# The Art Editor: a focused painting tool for 2D artists (same editor lib).
add_executable(HeartbreakArtEditor "Source/main_arteditor.cpp" "${HBE_SCHEMATIC_STUB}")
target_link_libraries(HeartbreakArtEditor PRIVATE hbe_editor)
set_target_properties(HeartbreakArtEditor PROPERTIES VS_DPI_AWARE "PerMonitor")


# Compile HLSL -> DXIL + SPIR-V and stage next to each executable.
include(ShaderCompile)
hbe_compile_engine_shaders(HeartbreakEditor)
hbe_compile_engine_shaders(HeartbreakRuntime)
hbe_compile_engine_shaders(HeartbreakArtEditor)

