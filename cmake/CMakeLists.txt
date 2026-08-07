cmake_minimum_required(VERSION 3.16)

include(.cmake.conf)
# Languages are enabled before find_package: Qt's config processing (e.g.
# find_package(Threads)) requires them, and CMake then exposes Qt6_VERSION so
# the module version can be derived from the found Qt before project() runs.
# QT_REPO_MODULE_VERSION (from .cmake.conf) supplies the baseline floor; its
# presence also keeps Qt's private-module autoload (Qt6CorePrivate etc.)
# active while the package is still only being configured.
enable_language(C CXX)
find_package(Qt6 ${QT_REPO_MODULE_VERSION} CONFIG REQUIRED
    COMPONENTS BuildInternals Core Gui Widgets
)
# Qt 6.9+ ships findable private module packages; Qt 6.8 does not. The
# ScriptTools module links Qt::GuiPrivate/Qt::WidgetsPrivate for historical
# parity, so request the packaged private modules when present and otherwise
# synthesize the private targets from their public modules (the module never
# includes Qt's private headers, so the include paths do not matter).
if(Qt6_VERSION VERSION_GREATER_EQUAL "6.9")
    find_package(Qt6 ${QT_REPO_MODULE_VERSION} CONFIG REQUIRED
        COMPONENTS CorePrivate GuiPrivate WidgetsPrivate
    )
else()
    foreach(pkg IN ITEMS CorePrivate GuiPrivate WidgetsPrivate)
        if(NOT TARGET Qt6::${pkg})
            string(REGEX REPLACE "Private$" "" pub "${pkg}")
            if(NOT TARGET Qt6::${pub})
                message(FATAL_ERROR
                    "Qt6::${pub} was not found; cannot provide Qt6::${pkg}.")
            endif()
            add_library(Qt6::${pkg} INTERFACE IMPORTED)
            set_target_properties(Qt6::${pkg} PROPERTIES
                INTERFACE_LINK_LIBRARIES Qt6::${pub})
        endif()
    endforeach()
endif()
# The module must carry the version of the Qt it is built against, whatever
# that is (CI covers 6.8.3 LTS and 6.11.1). Qt6_VERSION is the full x.y.z set
# by CMake's find_package from the Qt6 package version.
set(QT_REPO_MODULE_VERSION "${Qt6_VERSION}")
project(QtScript
    VERSION "${QT_REPO_MODULE_VERSION}"
    DESCRIPTION "Legacy Qt Script compatibility module"
    HOMEPAGE_URL "https://www.qt.io/"
    LANGUAGES C CXX
)

qt_internal_project_setup()
qt_build_repo()