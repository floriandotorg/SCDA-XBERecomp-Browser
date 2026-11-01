if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Use emcmake cmake for BROWSER_BUILD")
endif()

find_program(NODE_EXECUTABLE node REQUIRED)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} --version OUTPUT_VARIABLE compiler_banner COMMAND_ERROR_IS_FATAL ANY)
string(REGEX MATCH "emcc[^\n]*" compiler_identity "${compiler_banner}")
if(NOT compiler_identity MATCHES "6\\.0\\.13-git$")
    message(FATAL_ERROR "Browser toolchain is pinned to Emscripten 6.0.13-git; found ${compiler_identity}")
endif()

option(BROWSER_STORY "Compile and retain every generated story function" ON)
option(BROWSER_FULL_CPU "Build CPU contracts with all compact story functions and metadata" OFF)
option(BROWSER_BOOT "Build integrated story boot preflight" OFF)
set(BROWSER_GENERATION_RECORD "" CACHE FILEPATH "Trusted local generation reproduction record")
if(BROWSER_BOOT AND NOT BROWSER_GENERATION_RECORD)
    message(FATAL_ERROR "BROWSER_BOOT requires BROWSER_GENERATION_RECORD from verified generation reproduction")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(root ${PROJECT_SOURCE_DIR})
set(web ${CMAKE_BINARY_DIR}/web)
file(MAKE_DIRECTORY ${web})

set(story_sources ${root}/generated/game/x86_func_mapping.cpp)
foreach(n RANGE 0 127)
    list(APPEND story_sources ${root}/generated/game/x86_recomp.${n}.cpp)
endforeach()
set(identity_inputs ${story_sources} ${root}/CMakeLists.txt ${root}/src/browser/diagnostic_service.h
    ${root}/generated/game/x86_recomp_shared.h ${root}/generated/game/image_symbols.h
    ${root}/config/scda_v2.toml ${root}/config/xdk_symbols.toml
    ${root}/src/cpu/x86_ops.h ${root}/src/cpu/x86_context.h
    ${root}/src/browser/diagnostic.cpp ${root}/src/browser/placeholders.cpp
    ${root}/src/browser/index.html ${root}/src/browser/diagnostic.mjs
    ${CMAKE_CURRENT_SOURCE_DIR}/inventory.mjs ${CMAKE_CURRENT_LIST_FILE})
list(APPEND identity_inputs ${CMAKE_CURRENT_SOURCE_DIR}/provenance.mjs)
if(BROWSER_GENERATION_RECORD)
    list(APPEND identity_inputs ${BROWSER_GENERATION_RECORD} ${root}/assets/default.xbe)
    file(GLOB generator_inputs ${root}/tools/recomp/*.cpp ${root}/tools/recomp/*.h)
    list(APPEND identity_inputs ${generator_inputs} ${root}/src/xbe/xbe.h
        ${root}/generated/game/exports.def ${root}/generated/game/imports.def ${root}/generated/game/x86_recomp_stats.txt)
endif()
if(BROWSER_FULL_CPU OR BROWSER_BOOT)
    list(APPEND identity_inputs ${root}/src/browser/cpu_runtime.h ${root}/src/browser/cpu_runtime.cpp
        ${root}/tests/browser/cpu.cpp ${root}/tests/browser/full-cpu.html
        ${CMAKE_CURRENT_SOURCE_DIR}/adapt-memory.mjs ${CMAKE_CURRENT_SOURCE_DIR}/adapt-arithmetic.mjs
        ${CMAKE_CURRENT_SOURCE_DIR}/prepare-cpu.mjs)
endif()
set(boot_modules boot-worker.mjs boot-session.mjs boot-controller.mjs boot-runtime.mjs boot-shell.mjs
    boot-worker-broker.mjs boot-worker-host.mjs boot.html iso-reader.mjs iso-mailbox.mjs iso-wasm.mjs iso-io-service.mjs
    iso-io-worker.mjs iso-validation.mjs iso-image.mjs)
if(BROWSER_BOOT)
    file(GLOB kernel_declarations CONFIGURE_DEPENDS ${root}/src/kernel/*.cpp)
    list(APPEND identity_inputs ${kernel_declarations} ${CMAKE_CURRENT_SOURCE_DIR}/export-catalog.mjs)
    foreach(module IN LISTS boot_modules)
        list(APPEND identity_inputs ${root}/src/browser/${module})
    endforeach()
    list(APPEND identity_inputs ${root}/src/browser/boot-wasm.cpp ${root}/src/browser/disc-wasm.cpp
        ${root}/src/browser/image.cpp ${root}/src/browser/image.h
        ${root}/src/browser/kernel-provider.cpp ${root}/src/browser/kernel-provider.h
        ${root}/src/browser/virtual-memory.cpp ${root}/src/browser/virtual-memory.h
        ${root}/src/browser/contiguous-memory.cpp ${root}/src/browser/contiguous-memory.h
        ${root}/src/browser/string-provider.cpp ${root}/src/browser/string-provider.h
        ${root}/src/browser/section-provider.cpp ${root}/src/browser/section-provider.h
        ${root}/src/browser/file-provider.cpp ${root}/src/browser/file-provider.h
        ${root}/src/browser/temporary-filesystem.cpp ${root}/src/browser/temporary-filesystem.h
        ${root}/src/browser/raw-cache.cpp ${root}/src/browser/raw-cache.h
        ${root}/src/browser/cache-fatx.cpp ${root}/src/browser/cache-fatx.h
        ${root}/src/browser/thread-provider.cpp ${root}/src/browser/thread-provider.h
        ${root}/src/browser/platform-profile.cpp ${root}/src/browser/platform-profile.h
        ${root}/src/browser/shutdown-provider.cpp ${root}/src/browser/shutdown-provider.h
        ${root}/src/browser/dpc-provider.cpp ${root}/src/browser/dpc-provider.h
        ${root}/src/browser/timer-provider.cpp ${root}/src/browser/timer-provider.h
        ${root}/src/browser/sdk-provider.cpp ${root}/src/browser/sdk-provider.h
        ${root}/src/browser/diagnostic-graphics.cpp ${root}/src/browser/diagnostic-graphics.h
        ${root}/src/gpu/backend.h ${root}/src/gpu/state.h
        ${root}/src/browser/silent-audio.cpp ${root}/src/browser/silent-audio.h ${root}/src/audio/output.h
        ${root}/src/kernel/crypto.cpp ${root}/src/kernel/crypto.h ${root}/src/kernel/disc_index.cpp
        ${root}/src/kernel/disc_index.h ${root}/src/browser/iso-validation.mjs ${root}/src/browser/iso-image.mjs)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${identity_inputs})
execute_process(COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/inventory.mjs
    ${root} ${CMAKE_CURRENT_BINARY_DIR} "${compiler_identity}" "${CMAKE_BUILD_TYPE};story=${BROWSER_STORY};full-cpu=${BROWSER_FULL_CPU};boot=${BROWSER_BOOT}" "${BROWSER_GENERATION_RECORD}"
    COMMAND_ERROR_IS_FATAL ANY)

add_library(browser_flags INTERFACE)
target_compile_options(browser_flags INTERFACE -fwasm-exceptions -pthread -fno-strict-aliasing)
target_link_options(browser_flags INTERFACE -fwasm-exceptions -pthread -sMODULARIZE=1 -sEXPORT_ES6=1
    -sENVIRONMENT=web,node -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sMAXIMUM_MEMORY=268435456
    -sERROR_ON_UNDEFINED_SYMBOLS=1 -sFILESYSTEM=0)
target_include_directories(browser_flags INTERFACE ${root}/src ${CMAKE_CURRENT_BINARY_DIR})

add_executable(story_diagnostic ${root}/src/browser/diagnostic.cpp ${root}/src/browser/placeholders.cpp)
target_link_libraries(story_diagnostic PRIVATE browser_flags)
if(BROWSER_STORY)
    add_library(story_objects OBJECT ${story_sources})
    target_link_libraries(story_objects PRIVATE browser_flags)
    target_include_directories(story_objects PRIVATE ${root}/generated/game)
    target_compile_definitions(story_objects PRIVATE X86_GAME_CODE)
    target_compile_definitions(story_diagnostic PRIVATE BROWSER_STORY)
    target_link_libraries(story_diagnostic PRIVATE story_objects)
endif()
set_target_properties(story_diagnostic PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${web} SUFFIX .mjs)
configure_file(${root}/src/browser/index.html ${web}/index.html COPYONLY)
configure_file(${root}/src/browser/diagnostic.mjs ${web}/diagnostic.mjs COPYONLY)

if(BROWSER_FULL_CPU OR BROWSER_BOOT)
    set(compact ${CMAKE_CURRENT_BINARY_DIR}/compact-inputs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/adapt-memory.mjs ${CMAKE_CURRENT_SOURCE_DIR}/adapt-arithmetic.mjs
        ${CMAKE_CURRENT_SOURCE_DIR}/prepare-cpu.mjs)
    execute_process(COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/prepare-cpu.mjs ${root} ${compact}
        COMMAND_ERROR_IS_FATAL ANY)
    set(compact_sources ${compact}/x86_func_mapping.cpp)
    foreach(n RANGE 0 127)
        list(APPEND compact_sources ${compact}/x86_recomp.${n}.cpp)
    endforeach()
endif()

if(BROWSER_BOOT)
    add_executable(story_boot ${root}/src/browser/boot-wasm.cpp ${root}/src/browser/disc-wasm.cpp
        ${root}/src/browser/cpu_runtime.cpp ${root}/src/browser/kernel-provider.cpp ${root}/src/browser/virtual-memory.cpp ${root}/src/browser/contiguous-memory.cpp ${root}/src/browser/string-provider.cpp ${root}/src/browser/section-provider.cpp ${root}/src/browser/file-provider.cpp ${root}/src/browser/temporary-filesystem.cpp ${root}/src/browser/raw-cache.cpp ${root}/src/browser/cache-fatx.cpp ${root}/src/browser/thread-provider.cpp ${root}/src/kernel/crypto.cpp
        ${root}/src/browser/image.cpp ${root}/src/browser/platform-profile.cpp ${root}/src/browser/shutdown-provider.cpp ${root}/src/browser/dpc-provider.cpp ${root}/src/browser/timer-provider.cpp ${root}/src/browser/sdk-provider.cpp ${root}/src/browser/diagnostic-graphics.cpp ${root}/src/browser/silent-audio.cpp ${root}/src/kernel/disc_index.cpp ${compact_sources})
    target_link_libraries(story_boot PRIVATE browser_flags)
    target_include_directories(story_boot PRIVATE ${root}/generated/game)
    target_compile_definitions(story_boot PRIVATE X86_COMPACT_MEMORY)
    target_compile_options(story_boot PRIVATE -O3)
    target_link_options(story_boot PRIVATE --no-entry -sPTHREAD_POOL_SIZE=3 -sPTHREAD_POOL_SIZE_STRICT=2
        -sSTACK_SIZE=1048576 -sDEFAULT_PTHREAD_STACK_SIZE=262144 -sSTACK_OVERFLOW_CHECK=2
        -sEXPORTED_RUNTIME_METHODS=HEAPU8)
    set_target_properties(story_boot PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/boot-web SUFFIX .mjs)
    configure_file(${CMAKE_CURRENT_BINARY_DIR}/boot-manifest.mjs ${CMAKE_BINARY_DIR}/boot-web/boot-manifest.mjs COPYONLY)
    configure_file(${root}/src/browser/boot.html ${CMAKE_BINARY_DIR}/boot-web/index.html COPYONLY)
    foreach(module IN LISTS boot_modules)
        configure_file(${root}/src/browser/${module} ${CMAKE_BINARY_DIR}/boot-web/${module} COPYONLY)
    endforeach()
endif()

if(BROWSER_FULL_CPU)
    add_executable(cpu_contracts ${root}/tests/browser/cpu.cpp ${root}/src/browser/cpu_runtime.cpp ${compact_sources})
    target_include_directories(cpu_contracts PRIVATE ${root}/src ${root}/generated/game ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(cpu_contracts PRIVATE X86_COMPACT_MEMORY BROWSER_FULL_METADATA)
    target_compile_options(cpu_contracts PRIVATE -O3 -pthread -fwasm-exceptions -fno-strict-aliasing)
    target_link_options(cpu_contracts PRIVATE -pthread -fwasm-exceptions -sMODULARIZE=1 -sEXPORT_ES6=1
        -sENVIRONMENT=web,node -sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=0
        -sPTHREAD_POOL_SIZE=3 -sPTHREAD_POOL_SIZE_STRICT=2 -sPROXY_TO_PTHREAD=1
        -sSTACK_SIZE=1048576 -sDEFAULT_PTHREAD_STACK_SIZE=262144 -sSTACK_OVERFLOW_CHECK=2
        -sEXIT_RUNTIME=1 -sFILESYSTEM=0 -sERROR_ON_UNDEFINED_SYMBOLS=1)
    set_target_properties(cpu_contracts PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/cpu-web SUFFIX .mjs)
    configure_file(${root}/tests/browser/full-cpu.html ${CMAKE_BINARY_DIR}/cpu-web/index.html COPYONLY)
endif()
