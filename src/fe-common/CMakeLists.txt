add_library(fe_robustirc MODULE module-formats.c fe-robustirc.c)
target_link_libraries(fe_robustirc ${DEPS_LIBRARIES} m)
install(TARGETS fe_robustirc LIBRARY DESTINATION lib/irssi/modules)
