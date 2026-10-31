# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
cmake_minimum_required(VERSION 3.30.4)
project(cuvs_hnsw_sdk_pilot LANGUAGES CXX CUDA)

find_package(cuvs CONFIG REQUIRED)

add_executable(cuvs_hnsw_disk_gpu_pilot hnsw_disk_gpu_pilot.cu)
target_compile_features(cuvs_hnsw_disk_gpu_pilot PRIVATE cxx_std_20)
set_target_properties(
  cuvs_hnsw_disk_gpu_pilot
  PROPERTIES CUDA_STANDARD 20
             CUDA_STANDARD_REQUIRED ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
)
target_link_libraries(cuvs_hnsw_disk_gpu_pilot PRIVATE cuvs::cuvs)
