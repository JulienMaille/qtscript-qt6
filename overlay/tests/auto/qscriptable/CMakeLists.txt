qt_internal_add_test(tst_qscriptable SOURCES tst_qscriptable.cpp)
target_link_libraries(tst_qscriptable PRIVATE Qt6::Script)
