cmake_minimum_required(VERSION 3.16)

include(.cmake.conf)
project(QtScript
    DESCRIPTION "Legacy Qt Script compatibility module"
    HOMEPAGE_URL "https://www.qt.io/"
    LANGUAGES C CXX
)

find_package(Qt6 ${QT_REPO_MODULE_VERSION} CONFIG REQUIRED
    COMPONENTS BuildInternals Core Gui Widgets
)
find_package(Threads REQUIRED)

# Current macOS SDKs do not provide the legacy AGL framework. Qt's
# WrapOpenGL target can still propagate it when an out-of-tree Qt module is
# configured, even though QtGui already carries the runtime OpenGL linkage.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(wrap_opengl_libraries
        WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(wrap_opengl_libraries)
        list(FILTER wrap_opengl_libraries EXCLUDE REGEX "AGL(\\.framework)?$")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY
            INTERFACE_LINK_LIBRARIES "${wrap_opengl_libraries}")
    endif()
endif()

set(QTSCRIPT_QUICKJS_INCLUDE_DIR "" CACHE PATH
    "Path containing the pinned QuickJS-NG quickjs.h")
set(QTSCRIPT_QUICKJS_LIBRARY "" CACHE FILEPATH
    "Path to the configuration-matched static QuickJS-NG library")
if(NOT EXISTS "${QTSCRIPT_QUICKJS_INCLUDE_DIR}/quickjs.h")
    message(FATAL_ERROR
        "QTSCRIPT_QUICKJS_INCLUDE_DIR must point to the pinned QuickJS-NG source/include directory")
endif()
if(NOT EXISTS "${QTSCRIPT_QUICKJS_LIBRARY}")
    message(FATAL_ERROR
        "QTSCRIPT_QUICKJS_LIBRARY must point to the configuration-matched qjs static library")
endif()

add_library(QuickJS::QuickJS UNKNOWN IMPORTED GLOBAL)
set_target_properties(QuickJS::QuickJS PROPERTIES
    IMPORTED_LOCATION "${QTSCRIPT_QUICKJS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${QTSCRIPT_QUICKJS_INCLUDE_DIR}"
)
# The package must advertise the exact Qt version it was built against.  The
# value in .cmake.conf is only the minimum Qt version used for discovery.
set(QT_REPO_MODULE_VERSION "${Qt6_VERSION}")
set(PROJECT_VERSION "${Qt6_VERSION}")
set(PROJECT_VERSION_MAJOR "${Qt6_VERSION_MAJOR}")
set(PROJECT_VERSION_MINOR "${Qt6_VERSION_MINOR}")
set(PROJECT_VERSION_PATCH "${Qt6_VERSION_PATCH}")
set(QtScript_VERSION "${PROJECT_VERSION}")
set(QtScript_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(QtScript_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(QtScript_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
qt_internal_project_setup()

qt_build_repo()
