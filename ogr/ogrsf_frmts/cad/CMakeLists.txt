add_gdal_driver(
  TARGET ogr_CAD
  SOURCES ogr_cad.h
          gdalcaddataset.cpp
          ogrcaddriver.cpp
          ogrcadlayer.cpp
          vsilfileio.cpp
          vsilfileio.h
  CORE_SOURCES
          ogrcaddrivercore.cpp
  PLUGIN_CAPABLE
  NO_SHARED_SYMBOL_WITH_CORE
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
if(NOT TARGET ogr_CAD)
    return()
endif()

gdal_standard_includes(ogr_CAD)
target_compile_options(ogr_CAD PRIVATE ${GDAL_SOFTWARNFLAGS})

if (GDAL_USE_OPENCAD_INTERNAL)
  add_subdirectory(libopencad)
  target_compile_definitions(ogr_CAD PRIVATE "OCAD_EXTERN=")
  target_include_directories(ogr_CAD PRIVATE $<TARGET_PROPERTY:libopencad,SOURCE_DIR>)
else ()
  target_include_directories(ogr_CAD PRIVATE ${OPENCAD_INCLUDE_DIRS})
  gdal_target_link_libraries(ogr_CAD PRIVATE ${OPENCAD_LIBRARIES})
endif ()
