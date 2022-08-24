cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr)
project(my_echo_server)

target_sources(app PRIVATE src/echo-server.c)
target_sources(app PRIVATE src/tcp.c)
