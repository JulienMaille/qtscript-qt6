# Qt 6.8's macOS package can propagate the removed AGL framework through
# WrapOpenGL when a consumer loads Qt6ScriptTools. Keep this fix local to
# macOS and preserve every current framework dependency. This is the
# consumer-side mirror of the build-time strip in overlay/CMakeLists.txt;
# keep both in sync if Qt changes the WrapOpenGL linkage form.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(wrap_opengl_libraries
        WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(wrap_opengl_libraries)
        list(FILTER wrap_opengl_libraries EXCLUDE REGEX "AGL(\\.framework)?$")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY
            INTERFACE_LINK_LIBRARIES "${wrap_opengl_libraries}")
    endif()
endif()
