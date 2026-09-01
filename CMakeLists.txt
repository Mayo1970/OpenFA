cmake_minimum_required(VERSION 3.16)
project(fresh_adventures C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- engine core: zero external dependencies -------------------------------
# The core links no external library. The SDL2 desktop backend (RRR-41) is
# the one optional exception and it is isolated in its own translation unit:
# without SDL2 the file compiles to stubs and the null backend is used.
add_library(fa_core STATIC
  src/core/fa_loop.c
  src/core/fa_surface.c
  src/core/fa_script.c
  src/core/fa_aom.c
  src/core/fa_res.c
  src/core/fa_vfs.c
  src/core/fa_input.c
  src/core/fa_w01.c
  src/core/fa_w02.c
  src/core/fa_map.c
  src/core/fa_render.c
  src/core/fa_entity.c
  src/core/fa_bmp.c
  src/core/fa_menu.c
  src/core/fa_hiscore.c
  src/core/fa_options.c
  src/core/fa_save.c
  src/core/fa_wav.c
  src/core/fa_audio.c
  src/core/fa_rng.c
  src/game/fa_player.c
  src/game/fa_charspr.c
  src/game/fa_collide.c
  src/game/fa_beh.c
  src/game/fa_hud.c
  src/game/fa_death.c
  src/game/fa_credits.c
  src/platform/fa_platform.c
  src/platform/fa_backend_null.c
  src/platform/fa_backend_sdl2.c
  src/app/fa_app.c)
target_include_directories(fa_core PUBLIC include)

if (WIN32)
  target_sources(fa_core PRIVATE
    src/platform/fa_time_win32.c
    src/platform/fa_paths_win32.c)
  target_link_libraries(fa_core PUBLIC winmm)
else()
  target_sources(fa_core PRIVATE
    src/platform/fa_time_posix.c
    src/platform/fa_paths_posix.c)
  target_link_libraries(fa_core PUBLIC m)
endif()

# --- optional SDL2 desktop backend (RRR-41) ------------------------------
option(FA_WITH_SDL2 "Build the SDL2 desktop backend" ON)
if (FA_WITH_SDL2)
  find_package(SDL2 QUIET)
  if (SDL2_FOUND)
    target_compile_definitions(fa_core PRIVATE FA_HAVE_SDL2=1)
    if (TARGET SDL2::SDL2)
      target_link_libraries(fa_core PUBLIC SDL2::SDL2)
    else()
      target_include_directories(fa_core PRIVATE ${SDL2_INCLUDE_DIRS})
      target_link_libraries(fa_core PUBLIC ${SDL2_LIBRARIES})
    endif()
    message(STATUS "Fresh Adventures: SDL2 desktop backend enabled")
  else()
    message(STATUS "Fresh Adventures: SDL2 not found - null backend only")
  endif()
endif()

# --- headless deterministic driver + desktop slice ----------------------
add_executable(sim_replay tools/sim_replay.c)
target_link_libraries(sim_replay PRIVATE fa_core)

add_executable(fa_slice tools/fa_slice.c)
target_link_libraries(fa_slice PRIVATE fa_core)

add_executable(map_probe tools/map_probe.c)
target_link_libraries(map_probe PRIVATE fa_core)

# --- tests ---------------------------------------------------------------
enable_testing()
add_executable(test_loop tests/test_loop.c)
target_link_libraries(test_loop PRIVATE fa_core)
add_test(NAME loop COMMAND test_loop)

add_executable(test_surface tests/test_surface.c)
target_link_libraries(test_surface PRIVATE fa_core)
add_test(NAME surface COMMAND test_surface)

add_executable(test_script tests/test_script.c)
target_link_libraries(test_script PRIVATE fa_core)
# no argument: runs the Lua 4.0 semantics checks only. Set FA_GDATA to also
# evaluate the real EngineInit.jrs / DetailGroup.jrs (RRR-36 AC1).
if (DEFINED ENV{FA_GDATA})
  add_test(NAME script COMMAND test_script "$ENV{FA_GDATA}")
else()
  add_test(NAME script COMMAND test_script)
endif()

add_executable(test_aom tests/test_aom.c)
target_link_libraries(test_aom PRIVATE fa_core)
# no argument: synthetic-def checks only. Set FA_GDATA to also run the
# adler.jrs sample and the 128-script corpus sweep (RRR-37 AC3).
if (DEFINED ENV{FA_GDATA})
  add_test(NAME aom COMMAND test_aom "$ENV{FA_GDATA}")
else()
  add_test(NAME aom COMMAND test_aom)
endif()

add_executable(test_res tests/test_res.c)
target_link_libraries(test_res PRIVATE fa_core)
add_test(NAME res COMMAND test_res)

add_executable(test_vfs tests/test_vfs.c)
target_link_libraries(test_vfs PRIVATE fa_core)
add_test(NAME vfs COMMAND test_vfs)

add_executable(test_input tests/test_input.c)
target_link_libraries(test_input PRIVATE fa_core)
add_test(NAME input COMMAND test_input)

add_executable(test_platform tests/test_platform.c)
target_link_libraries(test_platform PRIVATE fa_core)
add_test(NAME platform COMMAND test_platform)

add_executable(test_map tests/test_map.c)
target_link_libraries(test_map PRIVATE fa_core)
# no argument: synthetic MapInfo / cell / camera checks. Set FA_GDATA to also
# load every real GData\Maps\*.W02 chunk 0 and render Welt2 (RRR-42).
if (DEFINED ENV{FA_GDATA})
  add_test(NAME map COMMAND test_map "$ENV{FA_GDATA}")
else()
  add_test(NAME map COMMAND test_map)
endif()

add_executable(test_entity tests/test_entity.c)
target_link_libraries(test_entity PRIVATE fa_core)
if (DEFINED ENV{FA_GDATA})
  add_test(NAME entity COMMAND test_entity "$ENV{FA_GDATA}")
else()
  add_test(NAME entity COMMAND test_entity)
endif()

add_executable(test_player tests/test_player.c)
target_link_libraries(test_player PRIVATE fa_core)
add_test(NAME player COMMAND test_player)

add_executable(test_beh tests/test_beh.c)
target_link_libraries(test_beh PRIVATE fa_core)
add_test(NAME beh COMMAND test_beh)

add_executable(test_rng tests/test_rng.c)
target_link_libraries(test_rng PRIVATE fa_core)
add_test(NAME rng COMMAND test_rng)

add_executable(test_death tests/test_death.c)
target_link_libraries(test_death PRIVATE fa_core)
add_test(NAME death COMMAND test_death)

add_executable(test_credits tests/test_credits.c)
target_link_libraries(test_credits PRIVATE fa_core)
add_test(NAME credits COMMAND test_credits)

add_executable(test_collide tests/test_collide.c)
target_link_libraries(test_collide PRIVATE fa_core)
add_test(NAME collide COMMAND test_collide)

add_executable(test_charspr tests/test_charspr.c)
target_link_libraries(test_charspr PRIVATE fa_core)
if (DEFINED ENV{FA_GDATA})
  add_test(NAME charspr COMMAND test_charspr "$ENV{FA_GDATA}")
else()
  add_test(NAME charspr COMMAND test_charspr)
endif()

add_executable(test_menu tests/test_menu.c)
target_link_libraries(test_menu PRIVATE fa_core)
if (DEFINED ENV{FA_GDATA})
  add_test(NAME menu COMMAND test_menu "$ENV{FA_GDATA}")
else()
  add_test(NAME menu COMMAND test_menu)
endif()

add_executable(test_save tests/test_save.c)
target_link_libraries(test_save PRIVATE fa_core)
add_test(NAME save COMMAND test_save)

add_executable(test_hiscore tests/test_hiscore.c)
target_link_libraries(test_hiscore PRIVATE fa_core)
if (DEFINED ENV{FA_GDATA})
  add_test(NAME hiscore COMMAND test_hiscore "$ENV{FA_GDATA}")
else()
  add_test(NAME hiscore COMMAND test_hiscore)
endif()

add_executable(test_audio tests/test_audio.c)
target_link_libraries(test_audio PRIVATE fa_core)
# no argument: synthetic WAV + mixer checks. Set FA_GDATA to also load a real
# shipped clip (SDat/Start.wav) and the music event path (RRR-46 AC3).
if (DEFINED ENV{FA_GDATA})
  add_test(NAME audio COMMAND test_audio "$ENV{FA_GDATA}")
else()
  add_test(NAME audio COMMAND test_audio)
endif()
