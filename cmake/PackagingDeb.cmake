if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_custom_target(package-deb
    COMMAND bash "${CMAKE_SOURCE_DIR}/packaging/linux/build-deb.sh"
      "${CMAKE_BINARY_DIR}"
    DEPENDS bongo_cat
    COMMENT "Building the BongoCat Linux Debian package"
    VERBATIM)
endif()
