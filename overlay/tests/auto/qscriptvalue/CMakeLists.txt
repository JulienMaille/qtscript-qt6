find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
qt_internal_add_test(tst_qscriptvalue SOURCES tst_qscriptvalue.cpp)
target_link_libraries(tst_qscriptvalue PRIVATE Qt6::Script Qt6::Widgets)
