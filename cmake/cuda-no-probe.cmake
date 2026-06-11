# Work around CMake failing to parse the nvcc -v link line with very new MSVC
# toolchains (e.g. VS 2026 / MSVC 195x against CUDA 12.x). Skip the compiler-id
# probe (CMAKE_CUDA_COMPILER_ID_RUN=1) and pre-seed the nvcc -v output that
# CMakeNVCCParseImplicitInfo expects.
#
# Toolkit root and version are derived from the active CUDA install (CUDA_PATH /
# nvcc) rather than hardcoded, so this follows whatever CUDA is installed
# (12.x, 13.x, ...). Once you move to a CUDA release that officially supports
# your compiler (CUDA >= 13.2 for VS 2026), the real probe would work too and
# this toolchain file simply becomes a harmless fast-path.

# --- 1. Locate the CUDA toolkit root ---------------------------------------
set(_cuda_root "")
if(DEFINED ENV{CUDA_PATH})
    file(TO_CMAKE_PATH "$ENV{CUDA_PATH}" _cuda_root)
elseif(DEFINED ENV{CUDAToolkit_ROOT})
    file(TO_CMAKE_PATH "$ENV{CUDAToolkit_ROOT}" _cuda_root)
else()
    find_program(_nvcc_on_path nvcc)
    if(_nvcc_on_path)
        get_filename_component(_cuda_bin  "${_nvcc_on_path}" DIRECTORY)
        get_filename_component(_cuda_root "${_cuda_bin}"     DIRECTORY)
    else()
        # Newest vNN.N folder under the default Windows install location.
        file(GLOB _cuda_dirs "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*")
        if(_cuda_dirs)
            list(SORT _cuda_dirs COMPARE NATURAL)
            list(REVERSE _cuda_dirs)            # highest version first
            list(GET _cuda_dirs 0 _cuda_root)
        endif()
    endif()
endif()

if(NOT _cuda_root OR NOT EXISTS "${_cuda_root}/bin/nvcc.exe")
    message(FATAL_ERROR
        "cuda-no-probe.cmake: could not locate a CUDA toolkit. "
        "Set the CUDA_PATH environment variable to the toolkit root "
        "(the folder containing bin/nvcc.exe).")
endif()

if(NOT CMAKE_CUDA_COMPILER)
    set(CMAKE_CUDA_COMPILER "${_cuda_root}/bin/nvcc.exe" CACHE FILEPATH "CUDA compiler")
endif()

# --- 2. Derive the toolkit version -----------------------------------------
# `nvcc --version` only prints the banner; it does NOT run the -v link probe
# this file exists to avoid, so it is safe to call at configure time.
#   ... "Cuda compilation tools, release 13.3, V13.3.52"
execute_process(
    COMMAND "${CMAKE_CUDA_COMPILER}" --version
    OUTPUT_VARIABLE _nvcc_banner
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "V([0-9]+\\.[0-9]+\\.[0-9]+)" _ "${_nvcc_banner}")
set(_cuda_version "${CMAKE_MATCH_1}")
if(NOT _cuda_version)
    message(FATAL_ERROR
        "cuda-no-probe.cmake: located CUDA at ${_cuda_root} but could not parse "
        "its version from `nvcc --version`.")
endif()

set(CMAKE_CUDA_COMPILER_ID "NVIDIA")
set(CMAKE_CUDA_COMPILER_VERSION "${_cuda_version}")
set(CMAKE_CUDA_COMPILER_WORKS TRUE)
set(CMAKE_CUDA_COMPILER_ID_RUN 1)
set(CMAKE_CUDA_COMPILER_FORCED TRUE)

# Skipping the compiler probe means CMake never computes the default language
# standard, so __compiler_check_default_language_standard() (called from
# Compiler/NVIDIA.cmake during project()) aborts with
# "CMAKE_CUDA_STANDARD_COMPUTED_DEFAULT ... should be set for NVIDIA".
# nvcc 12.x/13.x default to C++17 with extensions on; pre-seed both so project()
# passes. (CMAKE_CUDA_STANDARD is set to 17 in CMakeLists, so the std flag is
# emitted regardless of this default.)
set(CMAKE_CUDA_STANDARD_COMPUTED_DEFAULT "17")
set(CMAKE_CUDA_EXTENSIONS_COMPUTED_DEFAULT "ON")

set(CMAKE_CUDA_COMPILER_TOOLKIT_ROOT    "${_cuda_root}"    CACHE INTERNAL "")
set(CMAKE_CUDA_COMPILER_LIBRARY_ROOT    "${_cuda_root}"    CACHE INTERNAL "")
set(CMAKE_CUDA_COMPILER_TOOLKIT_VERSION "${_cuda_version}" CACHE INTERNAL "")

set(_cuda_libpath "/LIBPATH:${_cuda_root}/lib/x64")
string(APPEND _nvcc_fake_output "#$ LIBRARIES=  \"${_cuda_libpath}\"\n")
string(APPEND _nvcc_fake_output "#$ INCLUDES=\"-I${_cuda_root}/include\"\n")
string(APPEND _nvcc_fake_output "link.exe /OUT:CompilerIdCUDA.exe CompilerIdCUDA.obj \"${_cuda_libpath}\" cudart.lib\n")
set(CMAKE_CUDA_COMPILER_PRODUCED_OUTPUT "${_nvcc_fake_output}")

if(NOT CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "75;86;89;120" CACHE STRING "CUDA architectures")
endif()
set(CMAKE_CUDA_ARCHITECTURES_DEFAULT "89")

set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler" CACHE STRING "" FORCE)

unset(_cuda_root)
unset(_cuda_bin)
unset(_cuda_dirs)
unset(_nvcc_on_path CACHE)
unset(_cuda_libpath)
unset(_nvcc_fake_output)
unset(_nvcc_banner)
unset(_cuda_version)
