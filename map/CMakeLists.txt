project(map)

include_directories(
  ${OMIM_ROOT}/3party/protobuf/src
  ${OMIM_ROOT}/3party/freetype/include
  ${OMIM_ROOT}/3party/jansson/src
  ${OMIM_ROOT}/3party/glm
)

append(CMAKE_CXX_FLAGS "-std=c++11")

set(
  SRC
  ../api/src/c/api-client.c
  address_finder.cpp
  api_mark_point.cpp
  api_mark_point.hpp
  benchmark_tools.hpp
  benchmark_tools.cpp
  bookmark_manager.cpp
  bookmark_manager.hpp
  bookmark.cpp
  bookmark.hpp
  chart_generator.cpp
  chart_generator.hpp
  displacement_mode_manager.cpp
  displacement_mode_manager.hpp
  feature_vec_model.cpp
  feature_vec_model.hpp
  framework.cpp
  framework.hpp
  ge0_parser.cpp
  ge0_parser.hpp
  geourl_process.cpp
  geourl_process.hpp
  gps_track_collection.cpp
  gps_track_collection.hpp
  gps_track_filter.cpp
  gps_track_filter.hpp
  gps_track_storage.cpp
  gps_track_storage.hpp
  gps_track.cpp
  gps_track.hpp
  gps_tracker.cpp
  gps_tracker.hpp
  local_ads_manager.cpp
  local_ads_manager.hpp
  local_ads_supported_types.cpp
  mwm_tree.cpp
  mwm_tree.hpp
  mwm_url.cpp
  mwm_url.hpp
  place_page_info.cpp
  place_page_info.hpp
  reachable_by_taxi_checker.cpp
  reachable_by_taxi_checker.hpp
  track.cpp
  track.hpp
  traffic_manager.cpp
  traffic_manager.hpp
  user_mark_container.cpp
  user_mark_container.hpp
  user_mark.cpp
  user_mark.hpp
)

add_library(${PROJECT_NAME} ${SRC})

omim_add_test_subdirectory(map_tests)
omim_add_test_subdirectory(mwm_tests)
omim_add_test_subdirectory(style_tests)
