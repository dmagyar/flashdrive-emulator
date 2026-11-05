cmake_minimum_required(VERSION 3.16)

# SaMB client is a 2021-era userspace C library: it trips IDF's strict -Werror baseline
# (missing-field-initializers, array-parameter, implicit-fallthrough, old-style-definition).
# None of those are API problems, so relax them for that one component only - everything
# else in this build keeps IDF's full strictness. -Wno-error (not -w) keeps the warnings
# visible in the log so a real problem is still noticeable.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bridge)

idf_component_get_property(SMB_LIB sahlberg__libsmb2 COMPONENT_LIB)
target_compile_options(${SMB_LIB} PRIVATE -Wno-error)

# Measured below: 8 KiB SMB reads fit the 16 KiB receive window, but the window is only as
# good as the pbuf pool behind it. IDF never exposes PBUF_POOL_SIZE (opt.h, guarded), so
# override it here: 16 buffers x PBUF_POOL_BUFSIZE (~1480 B) = 23.7 KiB cannot back a
# 64 KiB window, and a segment with nowhere to land is dropped - which with SACK off means
# go-back-N from the gap. 48 buffers = ~71 KiB, enough to fill the window we advertise.
idf_component_get_property(LWIP_LIB lwip COMPONENT_LIB)
target_compile_definitions(${LWIP_LIB} PRIVATE PBUF_POOL_SIZE=48)
