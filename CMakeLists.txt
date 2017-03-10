cmake_minimum_required(VERSION 2.8)

option(USE_STATIC_LIBPNG "Use static libpng" ON)
if(USE_STATIC_LIBPNG)
  message(STATUS "- Using static libpng")
  SET (CMAKE_FIND_LIBRARY_SUFFIXES .a)
else()
  message(STATUS "- Using shared libpng")
endif()

FIND_PACKAGE(PNG 1.6.26 REQUIRED)

option(USE_STATIC_LIBM "Use static libm" ON)
if(NOT USE_STATIC_LIBM)
  message(STATUS "- Using static libm")
  SET (LIBM m)
else()
  message(STATUS "- Using sared libm")
  SET (LIBM libm.a)
endif()

add_executable(ply-image ply-image.c)
target_link_libraries(ply-image ${PNG_LIBRARIES} ${LIBM})

install(TARGETS ply-image DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
