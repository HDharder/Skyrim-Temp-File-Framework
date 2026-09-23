cmake_minimum_required(VERSION 3.21)

# The project name becomes the DLL name: TempFileFramework.dll.
# WARNING: include/TempFileAPI.h looks the DLL up by this name - changing it breaks consumers.
project(TempFileFramework VERSION 1.0.0 LANGUAGES CXX)

# Optional automatic deploy: SKYRIM_FOLDER (no mod manager) or
# SKYRIM_MODS_FOLDER (MO2/Vortex, goes to <mods>/TempFileFramework).
if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()
if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
endif()

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    src/plugin.cpp
    src/Store.cpp
    src/Hooks.cpp
    src/Exports.cpp
    src/Papyrus.cpp
)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" include src)
target_link_libraries(${PROJECT_NAME} PRIVATE minhook::minhook)

# Real Release PDB: without /Zi an MSVC Release build produces no PDB, leaving CrashLogger with
# no symbols matching the DLL.
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
target_link_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>>)

if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTPUT_FOLDER}/Scripts/Source"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/Source/TempFile.psc" "${OUTPUT_FOLDER}/Scripts/Source/TempFile.psc"
        VERBATIM
    )
endif()
