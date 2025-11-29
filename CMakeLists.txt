cmake_minimum_required(VERSION 3.5)
project(mod-real-online)

set(scripts_STAT_SRCS
  src/mod_real_online.cpp
  src/mod_token_level_milestones.cpp
  src/mod_token_login_streak.cpp
  src/autoupdate.cpp
)

AC_ADD_SCRIPT("${scripts_STAT_SRCS}")
AC_ADD_CONFIG_FILE(conf/mod_real_online.conf.dist)

message(STATUS "-> module 'mod-real-online' loaded.")
