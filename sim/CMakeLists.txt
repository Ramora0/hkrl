cmake_minimum_required(VERSION 3.20)
project(hksim C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# docs/float-parity.md: the float build contract; changing it changes every measured number
set(HKSIM_FP_FLAGS -msse2 -mfpmath=sse -ffp-contract=off -fno-fast-math
    -fexcess-precision=standard -frounding-math -fno-associative-math -fno-reciprocal-math)
# Python from PATH, or -DPython3_EXECUTABLE=<python>.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The scene / fsm-table / action registries are generated from the files on disk (sim/gen_registries.py).
# Runs every build and rewrites only on change.
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_registries.py" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "gen_registries.py failed (rc=${_rc}) using ${Python3_EXECUTABLE}")
endif()
add_custom_target(hksim_registries ALL COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_registries.py"
                  COMMENT "regenerating scene/action registries")

# Does every committed generated scene table still match the dump files under analysis/ it was generated
# from?  analysis/ is not tracked by git, so a replaced dump leaves no trace in `git status`.
# gate/inputs_fresh.py re-hashes each generator's recorded inputs (gate/tables_fresh.py is the full, slow
# regeneration).  HKSIM_SKIP_INPUTS_CHECK=1 skips it.
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../gate/inputs_fresh.py" RESULT_VARIABLE _irc)
if(NOT _irc EQUAL 0)
  message(FATAL_ERROR "inputs_fresh.py failed (rc=${_irc}): a committed scene table no longer matches "
          "the dump it was generated from (see output above). Regenerate the stale scene(s) and commit "
          "the result, or set HKSIM_SKIP_INPUTS_CHECK=1 to skip this check.")
endif()
add_custom_target(hksim_inputs_check ALL COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../gate/inputs_fresh.py"
                  COMMENT "checking committed scene tables against their recorded dump provenance")

file(GLOB_RECURSE HKSIM_SRC CONFIGURE_DEPENDS core/*.c hero/*.c phys/*.c fsm/*.c obs/*.c generated/*.c)
add_library(hksim SHARED ${HKSIM_SRC})
add_dependencies(hksim hksim_registries hksim_inputs_check)
target_compile_options(hksim PRIVATE ${HKSIM_FP_FLAGS} -O2 -Wall -Wextra -Werror=implicit-function-declaration)
target_include_directories(hksim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(hksim PROPERTIES PREFIX "" OUTPUT_NAME "hksim")
