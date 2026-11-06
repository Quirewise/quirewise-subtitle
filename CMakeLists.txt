cmake_minimum_required(VERSION 3.21)
project(QuirewiseSubtitle VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
file(GLOB_RECURSE QW_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)
add_library(quirewise_subtitle ${QW_LIBRARY_TYPE} ${QW_SOURCES})
add_library(Quirewise::Subtitle ALIAS quirewise_subtitle)
set_target_properties(quirewise_subtitle PROPERTIES EXPORT_NAME Subtitle POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_subtitle PUBLIC c_std_11)
target_include_directories(quirewise_subtitle PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/formats ${CMAKE_CURRENT_SOURCE_DIR}/src/backends)
install(TARGETS quirewise_subtitle EXPORT QuirewiseSubtitleTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewiseSubtitleTargets FILE QuirewiseSubtitleTargets.cmake NAMESPACE Quirewise:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseSubtitle")
configure_package_config_file(cmake/QuirewiseSubtitleConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseSubtitleConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseSubtitle")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/QuirewiseSubtitleConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseSubtitleConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseSubtitleConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseSubtitle")

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_subtitle
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
