cmake_minimum_required(VERSION 3.10)

project(ipc_lite C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(RKIPC_LITE_CROSS_COMPILE)
  message(STATUS "CROSS COMPILE TOOL: ${RKIPC_LITE_CROSS_COMPILE}")
endif()

add_compile_options(-Wall -Wextra -Wformat=2 -Werror=format-security)

add_executable(ipc_lite
  src/main.c
  src/app.c
  src/config.c
  src/log.c
  src/isp.c
  src/mpi_pipeline.c
  src/file_sink.c
  src/rtsp_sink.c
  src/rtmp_sink.c
  src/udp_rtp_sink.c
  src/wifi_status.c
)

add_executable(v4l2_mpi_venc_probe
  src/v4l2_mpi_venc_probe.c
  src/config.c
  src/isp.c
  src/log.c
  src/udp_rtp_sink.c
)

target_compile_definitions(ipc_lite PRIVATE RKAIQ UAPI2 RV1106_RV1103)
target_compile_definitions(v4l2_mpi_venc_probe PRIVATE RKAIQ UAPI2 RV1106_RV1103)

target_include_directories(ipc_lite PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(IPC_LITE_MEDIA_INCLUDE_DIRS)
  target_include_directories(ipc_lite PRIVATE ${IPC_LITE_MEDIA_INCLUDE_DIRS})
  target_include_directories(v4l2_mpi_venc_probe PRIVATE
    ${IPC_LITE_MEDIA_INCLUDE_DIRS}
  )
endif()

if(IPC_LITE_LINK_DIRS)
  target_link_directories(ipc_lite PRIVATE ${IPC_LITE_LINK_DIRS})
  target_link_directories(v4l2_mpi_venc_probe PRIVATE ${IPC_LITE_LINK_DIRS})
endif()

target_link_libraries(ipc_lite
  pthread
  m
  rockit
  rockchip_mpp
  rkaiq
  rtsp
  stdc++
)

target_link_libraries(v4l2_mpi_venc_probe
  pthread
  m
  rockit
  rkaiq
  stdc++
)

install(TARGETS ipc_lite DESTINATION ipc_lite)
install(TARGETS v4l2_mpi_venc_probe DESTINATION ipc_lite)
install(FILES
  ipc_lite.ini
  ipc_lite.720p.lowlat.rtsp.ini
  ipc_lite.720p60.fast.udp.ini
  ipc_lite.720p90.fast.udp.ini
  ipc_lite.1080p60.fast.udp.ini
  ipc_lite.832x464p120.fast.udp.ini
  DESTINATION ipc_lite
)
install(PROGRAMS run.sh DESTINATION ipc_lite)
