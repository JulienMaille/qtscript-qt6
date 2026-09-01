find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
qt_internal_add_test(tst_qscriptqwidgets SOURCES tst_qscriptqwidgets.cpp)
target_link_libraries(tst_qscriptqwidgets PRIVATE Qt6::Script Qt6::Widgets)
