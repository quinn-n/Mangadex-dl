# scripts/read-jobs.cmake: print this worktree's parallelism cap, or nothing.
#
#   cmake -P scripts/read-jobs.cmake              -> vendor.cpp-cli-studio.jobs
#   cmake -DKEY=load -P scripts/read-jobs.cmake   -> vendor.cpp-cli-studio.load
#
# Reads CMakeUserPresets.json at the repo root (written per worktree by /features):
#   "vendor": {"cpp-cli-studio": {"jobs": <int>, "load": <int>}}
# Prints a positive integer on stdout, or nothing when the file, the key or a sane value is
# missing. Exits 0 for any state of the file (FATAL only on a KEY other than jobs/load, which is
# caller misuse); the caller falls back to nproc.
cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED KEY)
  set(KEY jobs)
endif()
if(NOT KEY MATCHES "^(jobs|load)$")
  message(FATAL_ERROR "read-jobs.cmake: KEY must be 'jobs' or 'load', got '${KEY}'")
endif()

get_filename_component(_rj_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_rj_file "${_rj_root}/CMakeUserPresets.json")

if(EXISTS "${_rj_file}")
  file(READ "${_rj_file}" _rj_json)
  string(JSON _rj_value ERROR_VARIABLE _rj_err GET "${_rj_json}" vendor cpp-cli-studio ${KEY})
  if(_rj_value MATCHES "^[1-9][0-9]*$")
    # message() writes to stderr; echo through a child so the value lands on stdout.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_rj_value}")
  endif()
endif()
