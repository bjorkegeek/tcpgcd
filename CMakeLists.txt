cmake_minimum_required (VERSION 2.6)
project (tcpgcd)
add_executable(tcpgcd
  main.c
  opts.h opts.c
  protocol.h protocol.c
  ringbuf.h ringbuf.c
  sighandler.h sighandler.c
  sockutils.h sockutils.c
  listen.h listen.c
  connect.h connect.c
  )
