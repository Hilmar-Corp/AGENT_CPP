cmake_minimum_required(VERSION 3.20)
project(hilmarcorp_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(hilmarcorp-agent
  main.cpp
)

# We vendor nlohmann/json as: agent_cpp/third_party/nlohmann/json.hpp
# So the include should be: #include <nlohmann/json.hpp>
# and we add agent_cpp/third_party to the include path.

target_include_directories(hilmarcorp-agent PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}                 # agent_cpp/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party  # agent_cpp/third_party
)

target_link_libraries(hilmarcorp-agent PRIVATE
  CURL::libcurl
  OpenSSL::Crypto
)

if (MSVC)
  target_compile_options(hilmarcorp-agent PRIVATE /W4)
else()
  target_compile_options(hilmarcorp-agent PRIVATE -Wall -Wextra -Wpedantic)
endif()