qt_add_plugin(simplePlugin
    MODULE
    OUTPUT_NAME simpleplugin
    PLUGIN_TYPE script
    CLASS_NAME SimplePlugin
    simpleplugin/simpleplugin.cpp
)
target_link_libraries(simplePlugin PRIVATE Qt6::Script)
set_target_properties(simplePlugin PROPERTIES
    AUTOMOC ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/script"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/script"
)
foreach(config Debug Release)
    set_target_properties(simplePlugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}/plugins/script"
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}/plugins/script"
    )
endforeach()
add_custom_command(TARGET simplePlugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/plugins/script"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:simplePlugin>"
        "${CMAKE_CURRENT_BINARY_DIR}/plugins/script/$<TARGET_FILE_NAME:simplePlugin>"
)

qt6_add_resources(staticPluginResources staticplugin/staticplugin.qrc)
qt_add_plugin(staticPlugin
    STATIC
    OUTPUT_NAME staticplugin
    PLUGIN_TYPE script
    CLASS_NAME StaticPlugin
    staticplugin/staticplugin.cpp ${staticPluginResources}
)
target_link_libraries(staticPlugin PRIVATE Qt6::Script)
set_target_properties(staticPlugin PROPERTIES AUTOMOC ON)

qt_internal_add_test(tst_qscriptextensionplugin SOURCES tst_qscriptextensionplugin.cpp)
target_link_libraries(tst_qscriptextensionplugin PRIVATE Qt6::Script staticPlugin)
add_dependencies(tst_qscriptextensionplugin simplePlugin)
