find_package(Threads REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)

file(GLOB_RECURSE ${PROJECT}_SOURCES "*.cpp" "*.h" "*.rc")

function(header_directories RETURN_LIST)
    file(GLOB_RECURSE ALL_SRCS *.h *.cpp *.c)
    set(RELDIRS)
    foreach(SRC ${ALL_SRCS})
        file(RELATIVE_PATH RELSRC "${CMAKE_CURRENT_SOURCE_DIR}" "${SRC}")
        get_filename_component(RELDIR "${RELSRC}" DIRECTORY)
        list(APPEND RELDIRS ${RELDIR})
    endforeach()
    list(REMOVE_DUPLICATES RELDIRS)
    set(${RETURN_LIST} ${RELDIRS} PARENT_SCOPE)
endfunction()

header_directories(${PROJECT}_INCLUDES)

list(REMOVE_ITEM ${PROJECT}_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp" @ONLY)
list(APPEND ${PROJECT}_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp")

if(${PROJECT}_WITH_LEGACY_ASI)
    add_library(${EXECUTABLE} SHARED ${${PROJECT}_SOURCES})
else()
    add_executable(${EXECUTABLE} WIN32 ${${PROJECT}_SOURCES})
endif()

if(${PROJECT}_WITH_LEGACY_ASI)
    # The source glob includes extras/legacy_asi*.cpp; the opt-in macro is
    # private to the game core and must not change the renderer's ABI.
    target_compile_definitions(${EXECUTABLE} PRIVATE REVC_LEGACY_ASI REVC_LEGACY_ASI_CORE_DLL)
    target_link_options(${EXECUTABLE} PRIVATE /BASE:0x20000000)
    set_target_properties(${EXECUTABLE} PROPERTIES
        OUTPUT_NAME "reVC-core"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/legacy-asi"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/legacy-asi"
    )
    add_executable(reVC-legacy-bootstrap WIN32
        "${PROJECT_SOURCE_DIR}/utils/legacy-asi/launcher/bootstrap.cpp"
        "${PROJECT_SOURCE_DIR}/utils/legacy-asi/launcher/bootstrap_entry.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/extras/legacy_asi_identity.cpp")
    target_compile_definitions(reVC-legacy-bootstrap PRIVATE REVC_LEGACY_ASI)
    target_compile_features(reVC-legacy-bootstrap PRIVATE cxx_std_20)
    target_link_options(reVC-legacy-bootstrap PRIVATE /BASE:0x400000 /FIXED /DYNAMICBASE:NO
        /INCREMENTAL:NO /ENTRY:LegacyBootstrapEntry)
    set_source_files_properties("${PROJECT_SOURCE_DIR}/utils/legacy-asi/launcher/bootstrap_entry.cpp"
        PROPERTIES COMPILE_OPTIONS "/GS-;/JMC-")
    set_target_properties(reVC-legacy-bootstrap PROPERTIES OUTPUT_NAME "reVC-game"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/legacy-asi")
    add_executable(reVC-legacy-launcher WIN32
        "${PROJECT_SOURCE_DIR}/utils/legacy-asi/launcher/launcher.cpp"
        "${PROJECT_SOURCE_DIR}/utils/legacy-asi/launcher/debug_tls.cpp")
    target_compile_features(reVC-legacy-launcher PRIVATE cxx_std_20)
    target_link_libraries(reVC-legacy-launcher PRIVATE shell32 user32)
    set_target_properties(reVC-legacy-launcher PROPERTIES
        OUTPUT_NAME "reVC"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/legacy-asi")
    add_dependencies(${EXECUTABLE} reVC-legacy-launcher reVC-legacy-bootstrap)
endif()

target_link_libraries(${EXECUTABLE} PRIVATE
    librw::librw
    Threads::Threads
)
if(NOT ${PROJECT}_WITH_LEGACY_ASI OR NOT LIBRW_PLATFORM_D3D9)
    target_link_libraries(${EXECUTABLE} PRIVATE glfw)
endif()

target_include_directories(${EXECUTABLE}
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${${PROJECT}_INCLUDES}>
)

target_compile_definitions(${EXECUTABLE}
    PRIVATE
        $<IF:$<CONFIG:DEBUG>,DEBUG,NDEBUG>
        LIBRW
        CMAKE_NO_AUTOLINK
)

if(LIBRW_PLATFORM_D3D9)
    target_compile_definitions(${EXECUTABLE}
        PUBLIC
            USE_D3D9
    )
endif()

target_compile_definitions(${EXECUTABLE} PRIVATE CMAKE_BUILD)
target_compile_definitions(${EXECUTABLE} PRIVATE USE_OUR_VERSIONING)
if(NOT ${PROJECT}_WITH_MODLOADER)
    target_compile_definitions(${EXECUTABLE} PRIVATE REVC_NO_MODLOADER)
endif()

if(${PROJECT}_AUDIO STREQUAL "OAL")
    find_package(OpenAL REQUIRED)
    if(TARGET OpenAL::OpenAL)
        target_link_libraries(${EXECUTABLE} PRIVATE OpenAL::OpenAL)
    else()
        target_include_directories(${EXECUTABLE} PRIVATE ${OPENAL_INCLUDE_DIR})
        target_link_libraries(${EXECUTABLE} PRIVATE ${OPENAL_LIBRARY})
        target_compile_definitions(${EXECUTABLE} PRIVATE ${OPENAL_DEFINITIONS})
    endif()
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL)
elseif(${PROJECT}_AUDIO STREQUAL "MSS")
    find_package(MilesSDK REQUIRED)
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_MSS)
    target_link_libraries(${EXECUTABLE} PRIVATE MilesSDK::MilesSDK)
endif()

find_package(mpg123 REQUIRED)
target_link_libraries(${EXECUTABLE} PRIVATE
    MPG123::libmpg123
)
if(${PROJECT}_WITH_OPUS)
    find_package(opusfile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        opusfile::opusfile
    )
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OPUS)
endif()
if(${PROJECT}_WITH_LIBSNDFILE)
    find_package(SndFile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        SndFile::SndFile
    )
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL_USE_SNDFILE)
endif()

target_compile_definitions(${EXECUTABLE} PRIVATE )

option(${PROJECT}_WITH_SANITIZERS "Use UB sanitizers (better crash log)" OFF)
option(${PROJECT}_WITH_ASAN "Use Address sanitizer (better crash log)" OFF)

if(${PROJECT}_WITH_SANITIZERS)
    target_compile_options(${EXECUTABLE} PUBLIC
            -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,implicit-integer-truncation,implicit-integer-arithmetic-value-change,local-bounds,nullability
            -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,implicit-integer-truncation,implicit-integer-arithmetic-value-change,local-bounds,nullability)
endif()

if(${PROJECT}_WITH_ASAN)
    target_compile_options(${EXECUTABLE} PUBLIC -fsanitize=address -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC -fsanitize=address)
endif()

if(${PROJECT}_WITH_DISCORD_RPC)
    target_compile_definitions(${EXECUTABLE} PRIVATE USE_DISCORD_RPC)
    target_include_directories(${EXECUTABLE} PRIVATE "${DISCORD_RPC_DIR}/include")
    target_link_libraries(${EXECUTABLE} PRIVATE discord-rpc)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(${EXECUTABLE}
        PRIVATE
            "-Wall"
    )
    if (NOT LIBRW_PLATFORM_PS2)
        target_compile_options(${EXECUTABLE}
            PRIVATE
                -Wextra
                -Wdouble-promotion
                -Wpedantic
        )
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${EXECUTABLE}
        PUBLIC
            /Zc:sizedDealloc-
    )
endif()

if(NINTENDO_SWITCH)
    set(${PROJECT}_C_CXX_EXTENSIONS ON)
else()
    set(${PROJECT}_C_CXX_EXTENSIONS OFF)
endif()

if(LIBRW_PLATFORM_GL3 AND LIBRW_GL3_GFXLIB STREQUAL "GLFW")
	include(CheckSymbolExists)

	set(CMAKE_REQUIRED_LIBRARIES glfw)
	set(CMAKE_REQUIRED_DEFINITIONS -DGLFW_EXPOSE_NATIVE_X11)
	check_symbol_exists(glfwGetX11Display "GLFW/glfw3.h;GLFW/glfw3native.h" GLFW_HAS_X11)
	unset(CMAKE_REQUIRED_DEFINITIONS)
	unset(CMAKE_REQUIRED_LIBRARIES)

	if (GLFW_HAS_X11)
		find_package(X11 REQUIRED)
		target_link_libraries(${EXECUTABLE} PRIVATE X11::X11)
		target_compile_definitions(${EXECUTABLE} PRIVATE GET_KEYBOARD_INPUT_FROM_X11)
	endif (GLFW_HAS_X11)
endif()

set_target_properties(${EXECUTABLE}
    PROPERTIES
        C_STANDARD 11
        C_EXTENSIONS ${${PROJECT}_C_CXX_EXTENSIONS}
        C_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ${${PROJECT}_C_CXX_EXTENSIONS}
        CXX_STANDARD_REQUIRED ON
)
target_compile_features(${EXECUTABLE} PRIVATE cxx_std_23)

if(${PROJECT}_INSTALL)
    if(${PROJECT}_WITH_LEGACY_ASI)
        install(TARGETS reVC-legacy-launcher reVC-legacy-bootstrap RUNTIME DESTINATION ".")
    endif()
    install(
        TARGETS ${EXECUTABLE}
        EXPORT ${EXECUTABLE}-targets
        RUNTIME DESTINATION "."
    )
    if(MSVC)
        install(FILES $<TARGET_PDB_FILE:${EXECUTABLE}> DESTINATION "." OPTIONAL)
    endif()
endif()

reVC_platform_target(${EXECUTABLE} INSTALL)
