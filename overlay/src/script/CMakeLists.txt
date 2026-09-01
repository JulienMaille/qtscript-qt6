set(api_sources
    api/qregexp.cpp
    api/qscriptextensionplugin.cpp
)

set(syntax_checker_sources
    parser/qscriptgrammar.cpp
    parser/qscriptlexer.cpp
    parser/qscriptsyntaxchecker.cpp
)

set(quickjs_backend_sources
    quickjs/qscriptenginequickjs.cpp
    quickjs/qscriptvaluequickjs.cpp
    quickjs/qscriptvalueiteratorquickjs.cpp
    quickjs/qscriptqobjectquickjs.cpp
    quickjs/qscriptablequickjs.cpp
    quickjs/qscriptclassquickjs.cpp
    quickjs/qscriptcontextinfoquickjs.cpp
    quickjs/qscriptdeclarativeclassquickjs.cpp
)

set(public_headers
    api/qregexp.h
    api/qscriptable.h
    api/qscriptclass.h
    api/qscriptclasspropertyiterator.h
    api/qscriptcontext.h
    api/qscriptcontextinfo.h
    api/qscriptengine.h
    api/qscriptengineagent.h
    api/qscriptextensioninterface.h
    api/qscriptextensionplugin.h
    api/qscriptprogram.h
    api/qscriptstring.h
    api/qscriptvalue.h
    api/qscriptvalueiterator.h
    api/qtscriptglobal.h
)

set(private_headers
    bridge/qscriptdeclarativeclass_p.h
)

qt_internal_add_module(Script
    NO_UNITY_BUILD
    SOURCES
        ${api_sources}
        ${syntax_checker_sources}
        ${quickjs_backend_sources}
        ${public_headers}
        ${private_headers}
    DEFINES
        BUILDING_QT__=1
        QT_NO_USING_NAMESPACE
        QTSCRIPT_USE_QUICKJS=1
    INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/api"
        "${CMAKE_CURRENT_SOURCE_DIR}/parser"
        "${CMAKE_CURRENT_SOURCE_DIR}/quickjs"
    LIBRARIES
        Qt::CorePrivate
        QuickJS::QuickJS
        Threads::Threads
        ${CMAKE_DL_LIBS}
    PUBLIC_LIBRARIES
        Qt::Core
    PRIVATE_MODULE_INTERFACE
        Qt::CorePrivate
)

if(UNIX AND NOT APPLE)
    target_link_libraries(Script PRIVATE m)
endif()

qt_internal_extend_target(Script CONDITION WIN32
    DEFINES
        NOMINMAX
)
