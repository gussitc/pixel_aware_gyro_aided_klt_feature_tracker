cmake_minimum_required(VERSION 3.0.2)
project(gyro_aided_feature_tracker)

add_compile_options(-std=c++11)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}  -Wall  -O0 -g")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall  -O0 -g")
else()
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}  -Wall  -O3")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall  -O3")
endif()

find_package(catkin REQUIRED COMPONENTS
  cv_bridge
  rosbag
  roscpp
  sensor_msgs
)

include_directories(
${catkin_INCLUDE_DIRS}
${PROJECT_SOURCE_DIR}
${PROJECT_SOURCE_DIR}/../../../
${PROJECT_SOURCE_DIR}/../../../src
${PROJECT_SOURCE_DIR}/../../../include
)

set(LINK_LIBS
${catkin_LIBRARIES}
pixel_aware_gyro_aided_klt_feature_tracker
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE})

add_executable(${PROJECT_NAME} src/feature_tracker.cpp)

target_link_libraries(${PROJECT_NAME}
${LINK_LIBS})