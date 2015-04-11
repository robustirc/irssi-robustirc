set(SOURCE
   ${SOURCE}
   ${CMAKE_CURRENT_SOURCE_DIR}/robustirc.c)
set(HEADERS
   ${HEADERS}
   ${CMAKE_CURRENT_SOURCE_DIR}/robustirc.h)
add_subdirectory("robustsession")
add_library(robustirc_core MODULE ${SOURCE} ${HEADERS})
target_link_libraries(robustirc_core ${DEPS_LIBRARIES} m)
install(TARGETS robustirc_core LIBRARY DESTINATION lib/irssi/modules)
