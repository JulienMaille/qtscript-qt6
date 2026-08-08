set(quickjs_dir "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/quickjs")

set(api_sources
    api/qscriptengine.cpp
    api/qscriptvalue.cpp
    api/qregexp.cpp
)

set(bridge_sources
    bridge/qobject_bridge.cpp
)

set(quickjs_sources
    "${quickjs_dir}/quickjs.c"
    "${quickjs_dir}/cutils.c"
    "${quickjs_dir}/libregexp.c"
    "${quickjs_dir}/libunicode.c"
    "${quickjs_dir}/libbf.c"
)

set(api_headers
    api/qscriptengine.h
    api/qscriptvalue.h
    api/qregexp.h
    api/qtscriptglobal.h
)

qt_internal_add_module(Script
    NO_UNITY_BUILD
    SOURCES
        ${api_sources}
        ${bridge_sources}
        ${quickjs_sources}
        ${api_headers}
    DEFINES
        BUILDING_QT__=1
    INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/api"
        "${quickjs_dir}"
    LIBRARIES
        Qt::CorePrivate
    PUBLIC_LIBRARIES
        Qt::Core
    PRIVATE_MODULE_INTERFACE
        Qt::CorePrivate
)

target_compile_definitions(Script PRIVATE CONFIG_VERSION="2024-01-13")
