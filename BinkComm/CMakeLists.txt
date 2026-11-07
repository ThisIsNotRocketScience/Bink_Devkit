cmake_minimum_required(VERSION 3.13)
project(BinkComm CXX)

add_library(BinkComm STATIC src/BinkComm.cpp)
target_include_directories(BinkComm PUBLIC include)
target_compile_features(BinkComm PUBLIC cxx_std_11)
