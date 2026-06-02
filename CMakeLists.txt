cmake_minimum_required(VERSION 3.18)

# CUDA 12.x's nvcc refuses cl.exe >= 19.40 by default ("unsupported Microsoft
# Visual Studio version"). The C1189 it emits is a soft check -- the codegen
# itself works fine. We pre-seed CMAKE_CUDA_FLAGS so the flag applies even to
# the compiler-identification probe that runs during project() below.
if(NOT DEFINED CMAKE_CUDA_FLAGS OR NOT "${CMAKE_CUDA_FLAGS}" MATCHES "allow-unsupported-compiler")
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler")
endif()

project(sdr2hdr LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
# Ampere (RTX 30), Ada (RTX 40), Blackwell (RTX 50). 75 = Turing (RTX 20/GTX 16).
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "75;86;89;120")
endif()

# ---------------------------------------------------------------------------
# Resolve SDK root. Prefer $ENV{NV_RTX_VIDEO_SDK}; fall back to ../..  of this
# file (because the tool lives inside the SDK tree at tools/sdr2hdr/).
# ---------------------------------------------------------------------------
if(DEFINED ENV{NV_RTX_VIDEO_SDK})
    set(RTX_SDK_ROOT "$ENV{NV_RTX_VIDEO_SDK}")
else()
    get_filename_component(RTX_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

message(STATUS "RTX Video SDK root: ${RTX_SDK_ROOT}")

if(NOT EXISTS "${RTX_SDK_ROOT}/include/nvsdk_ngx.h")
    message(FATAL_ERROR
        "Could not locate the RTX Video SDK. "
        "Set the NV_RTX_VIDEO_SDK environment variable to the SDK directory "
        "(the folder containing bin/, include/, lib/, samples/).")
endif()

# ---------------------------------------------------------------------------
# CUDA runtime + driver API
# ---------------------------------------------------------------------------
find_package(CUDAToolkit REQUIRED)

# ---------------------------------------------------------------------------
# NVIDIA Video Codec SDK (vendored under tools/sdr2hdr/vendor/nvcodec).
# We compile the sample NvDecoder/NvEncoder wrappers directly into the exe so
# we don't need a pre-built SDK static lib. Layout under vendor mirrors the
# upstream SDK so the sample sources' relative includes still resolve.
# ---------------------------------------------------------------------------
if(DEFINED ENV{NV_VIDEO_CODEC_SDK})
    set(NVCODEC_ROOT "$ENV{NV_VIDEO_CODEC_SDK}")
else()
    set(NVCODEC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvcodec")
endif()

message(STATUS "Video Codec SDK root: ${NVCODEC_ROOT}")

if(NOT EXISTS "${NVCODEC_ROOT}/Interface/nvcuvid.h")
    message(FATAL_ERROR
        "Could not locate the Video Codec SDK at ${NVCODEC_ROOT}. "
        "Set the NV_VIDEO_CODEC_SDK environment variable to the SDK directory "
        "(the folder containing Interface/, Lib/, Samples/), "
        "or place it at vendor/nvcodec/.")
endif()

set(NVCODEC_SAMPLES   "${NVCODEC_ROOT}/Samples")
set(NVCODEC_WRAP_SRC
    "${NVCODEC_SAMPLES}/NvCodec/NvDecoder/NvDecoder.cpp"
    "${NVCODEC_SAMPLES}/NvCodec/NvEncoder/NvEncoder.cpp"
    "${NVCODEC_SAMPLES}/NvCodec/NvEncoder/NvEncoderCuda.cpp"
)

# ---------------------------------------------------------------------------
# RTX_Video_API source files.
# ---------------------------------------------------------------------------
set(RTX_API_DIR "${RTX_SDK_ROOT}/samples/RTX_Video_API")

add_executable(sdr2hdr
    src/main.cpp
    src/rtx_converter.cpp
    src/ffmpeg_process.cpp
    src/bitstream_pipe.cpp
    src/gpu_pipeline.cpp
    src/win32_utils.cpp
    src/deps.cpp
    src/cuda_kernels.cu
    "${RTX_API_DIR}/rtx_video_api_cuda_impl.cpp"
    ${NVCODEC_WRAP_SRC}
)

target_include_directories(sdr2hdr PRIVATE
    src
    "${RTX_SDK_ROOT}/include"
    "${RTX_API_DIR}"
    "${NVCODEC_ROOT}/Interface"
    # Needed so "../Utils/NvCodecUtils.h" in NvDecoder.h / NvEncoder.h resolves
    # to Samples/Utils/NvCodecUtils.h via the compiler's -I fallback search.
    "${NVCODEC_SAMPLES}/NvCodec"
    "${NVCODEC_SAMPLES}/Utils"
)

target_link_directories(sdr2hdr PRIVATE
    "${RTX_SDK_ROOT}/lib/Windows/x64"
    "${NVCODEC_ROOT}/Lib/win/x64"
)

target_link_libraries(sdr2hdr PRIVATE
    CUDA::cuda_driver
    CUDA::cudart
    nvcuvid
    nvencodeapi
)

target_compile_features(sdr2hdr PRIVATE cxx_std_17)

# The NvCodec sample wrappers emit "'sprintf': deprecated", many
# conversion / unreferenced-parameter warnings. Silence them so our own
# build output stays readable. They're upstream code; we don't patch.
set_source_files_properties(${NVCODEC_WRAP_SRC} PROPERTIES
    COMPILE_FLAGS "/W0 /wd4819"
)

if(MSVC)
    target_compile_options(sdr2hdr PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W3>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        # /bigobj needed because NvEncoder.cpp has many template instantiations.
        $<$<COMPILE_LANGUAGE:CXX>:/bigobj>
        # main.cpp embeds Chinese string literals (i18n wizard translations).
        # Without /utf-8 MSVC reads the source as the system ACP (CP936 on
        # Chinese Windows) and bakes wrong bytes into the binary, producing
        # mojibake on even UTF-8-enabled consoles. /utf-8 = /source-charset:utf-8
        # + /execution-charset:utf-8 which matches how we treat all strings.
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    )
    target_compile_definitions(sdr2hdr PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    # The RTX Video SDK ships /MD (dynamic runtime) default libs. Match that
    # unless the user overrides CMAKE_MSVC_RUNTIME_LIBRARY at configure time.
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set_property(TARGET sdr2hdr PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

# ---------------------------------------------------------------------------
# Copy required NVNGX feature DLLs next to the executable so the tool runs
# out-of-the-box. Both TrueHDR and VSR are needed to support all three modes.
#
# The SDK ships two flavours of each DLL:
#   bin/Windows/x64/dev/   -- development build; stamps a big watermark plus
#                             "DO NOT DISTRIBUTE / USE RELEASE VERSION" on every
#                             processed frame. Handy for debugging, but ruins
#                             the output for actual use.
#   bin/Windows/x64/rel/   -- release build; clean output, no watermark.
#
# We deliberately bundle the release DLLs. Override with -DRTX_DLL_FLAVOUR=dev
# if you ever need the diagnostic overlay back.
# ---------------------------------------------------------------------------
set(RTX_DLL_FLAVOUR "rel" CACHE STRING "RTX Video NVNGX DLL flavour: rel | dev")
set(NGX_SRC_DIR "${RTX_SDK_ROOT}/bin/Windows/x64/${RTX_DLL_FLAVOUR}")
if(NOT EXISTS "${NGX_SRC_DIR}/nvngx_truehdr.dll")
    message(FATAL_ERROR
        "nvngx_truehdr.dll not found in ${NGX_SRC_DIR}. "
        "Set -DRTX_DLL_FLAVOUR=<subfolder under bin/Windows/x64/> to pick another.")
endif()
message(STATUS "RTX NVNGX DLL flavour: ${RTX_DLL_FLAVOUR} (${NGX_SRC_DIR})")

foreach(dll_name nvngx_truehdr.dll nvngx_vsr.dll)
    add_custom_command(TARGET sdr2hdr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NGX_SRC_DIR}/${dll_name}"
            "$<TARGET_FILE_DIR:sdr2hdr>/${dll_name}"
        COMMENT "Copying ${dll_name} (${RTX_DLL_FLAVOUR}) next to sdr2hdr.exe")
endforeach()

# ---------------------------------------------------------------------------
# Optionally stage ffmpeg.exe + ffprobe.exe next to sdr2hdr.exe so the tool
# is fully self-contained. At runtime sdr2hdr.exe prefers its own directory
# over PATH for these binaries.
#
#   -DBUNDLE_FFMPEG=ON        : force staging (fail if ffmpeg not found)
#   -DBUNDLE_FFMPEG=AUTO      : stage if ffmpeg is on PATH (default)
#   -DBUNDLE_FFMPEG=OFF       : never stage
# ---------------------------------------------------------------------------
if(NOT DEFINED BUNDLE_FFMPEG)
    set(BUNDLE_FFMPEG "AUTO" CACHE STRING "Bundle ffmpeg.exe/ffprobe.exe: ON | AUTO | OFF")
endif()

if(NOT BUNDLE_FFMPEG STREQUAL "OFF")
    find_program(FFMPEG_EXE  ffmpeg)
    find_program(FFPROBE_EXE ffprobe)
    if(FFMPEG_EXE AND FFPROBE_EXE)
        message(STATUS "Bundling ffmpeg:  ${FFMPEG_EXE}")
        message(STATUS "Bundling ffprobe: ${FFPROBE_EXE}")
        add_custom_command(TARGET sdr2hdr POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_EXE}"  "$<TARGET_FILE_DIR:sdr2hdr>/ffmpeg.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFPROBE_EXE}" "$<TARGET_FILE_DIR:sdr2hdr>/ffprobe.exe"
            COMMENT "Bundling ffmpeg.exe + ffprobe.exe next to sdr2hdr.exe")
    elseif(BUNDLE_FFMPEG STREQUAL "ON")
        message(FATAL_ERROR
            "BUNDLE_FFMPEG=ON but ffmpeg or ffprobe were not found on PATH.")
    else()
        message(STATUS "ffmpeg not found on PATH - sdr2hdr.exe will rely on PATH at runtime.")
    endif()
endif()
