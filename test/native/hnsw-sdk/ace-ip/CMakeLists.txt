# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
cmake_minimum_required(VERSION 3.30.4)
project(cuvs_hnsw_ace_ip_metric LANGUAGES CXX CUDA)

find_package(cuvs CONFIG REQUIRED)

get_filename_component(CUVS_PATCH_SOURCE_CPP "${CMAKE_CURRENT_LIST_DIR}/../../../../cpp/src" ABSOLUTE)
add_executable(cuvs_hnsw_ace_ip_serializer_policy_cpu_probe serializer_builder_policy_cpu_probe.cpp)
target_compile_features(cuvs_hnsw_ace_ip_serializer_policy_cpu_probe PRIVATE cxx_std_20)
target_include_directories(cuvs_hnsw_ace_ip_serializer_policy_cpu_probe PRIVATE "${CUVS_PATCH_SOURCE_CPP}")

add_executable(cuvs_hnsw_ace_partition_budget_model_probe ace_partition_budget_model_probe.cpp)
target_compile_features(cuvs_hnsw_ace_partition_budget_model_probe PRIVATE cxx_std_20)
target_include_directories(cuvs_hnsw_ace_partition_budget_model_probe PRIVATE "${CUVS_PATCH_SOURCE_CPP}")

add_executable(cuvs_hnsw_ace_ip_factory_metric_probe factory_metric_probe.cpp)
target_compile_features(cuvs_hnsw_ace_ip_factory_metric_probe PRIVATE cxx_std_20)
target_link_libraries(cuvs_hnsw_ace_ip_factory_metric_probe PRIVATE cuvs::cuvs)

add_executable(cuvs_hnsw_ace_subbuilder_budget_probe ace_subbuilder_budget_probe.cu)
target_compile_features(cuvs_hnsw_ace_subbuilder_budget_probe PRIVATE cxx_std_20)
set_target_properties(
  cuvs_hnsw_ace_subbuilder_budget_probe
  PROPERTIES CUDA_STANDARD 20
             CUDA_STANDARD_REQUIRED ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
)
target_link_libraries(cuvs_hnsw_ace_subbuilder_budget_probe PRIVATE cuvs::cuvs)

add_executable(cuvs_hnsw_ace_ip_quality_pilot ace_ip_quality_pilot.cu)
target_compile_features(cuvs_hnsw_ace_ip_quality_pilot PRIVATE cxx_std_20)
set_target_properties(
  cuvs_hnsw_ace_ip_quality_pilot
  PROPERTIES CUDA_STANDARD 20
             CUDA_STANDARD_REQUIRED ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
)
target_link_libraries(cuvs_hnsw_ace_ip_quality_pilot PRIVATE cuvs::cuvs)
