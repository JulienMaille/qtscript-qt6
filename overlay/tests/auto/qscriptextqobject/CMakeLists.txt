find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
qt_internal_add_test(tst_qscriptextqobject SOURCES tst_qscriptextqobject.cpp)
target_link_libraries(tst_qscriptextqobject PRIVATE Qt6::Script Qt6::Widgets)
