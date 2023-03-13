cmake_minimum_required(VERSION 2.4.6)
# include($ENV{ROS_ROOT}/core/rosbuild/rosbuild.cmake)
project(feature_tracker)

# rosbuild_init()
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../)

find_package(catkin COMPONENTS roscpp sensor_msgs rosbag cv_bridge REQUIRED)

# IF(NOT ROS_BUILD_TYPE)
#   SET(ROS_BUILD_TYPE Release)
# ENDIF()

# MESSAGE("Build type: " ${ROS_BUILD_TYPE})
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}  -Wall  -O0 -g")
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall  -O0 -g")
else()
   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}  -Wall  -O3")
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall  -O3")
endif()

# Check C++11 or C++0x support
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
CHECK_CXX_COMPILER_FLAG("-std=c++0x" COMPILER_SUPPORTS_CXX0X)
if(COMPILER_SUPPORTS_CXX11)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
   add_definitions(-DCOMPILEDWITHC11)
   message(STATUS "Using flag -std=c++11.")
elseif(COMPILER_SUPPORTS_CXX0X)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
   add_definitions(-DCOMPILEDWITHC0X)
   message(STATUS "Using flag -std=c++0x.")
else()
   message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()


find_package(OpenCV 4.2 REQUIRED)
if(NOT OpenCV_FOUND)
   find_package(OpenCV 2.4.3 QUIET)
   if(NOT OpenCV_FOUND)
      message(FATAL_ERROR "OpenCV > 2.4.3 not found.")
   endif()
endif()

# find_package(rosbag REQUIRED )
find_package(Eigen3 3.1.0 REQUIRED)

include_directories(
# ${rosbag_INCLUDE_DIRS}
${catkin_INCLUDE_DIRS}
${PROJECT_SOURCE_DIR}
${PROJECT_SOURCE_DIR}/../../../
${PROJECT_SOURCE_DIR}/../../../src
${PROJECT_SOURCE_DIR}/../../../include
)

set(LINK_LIBS
# ${rosbag_LIBRARIES}
${catkin_LIBRARIES}
${OpenCV_LIBS}
${EIGEN3_LIBS}
${PROJECT_SOURCE_DIR}/../../../Thirdparty/glog/lib/libglog.so
# ${PROJECT_SOURCE_DIR}/../../../lib/libpixel_aware_gyro_aided_klt_feature_tracker.so
pixel_aware_gyro_aided_klt_feature_tracker
)


set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

# feature_tracker
# rosbuild_add_executable(feature_tracker
# #include/common.h
# src/feature_tracker.cpp )

add_executable(${PROJECT_NAME} src/feature_tracker.cpp)

target_link_libraries(${PROJECT_NAME}
${LINK_LIBS})
