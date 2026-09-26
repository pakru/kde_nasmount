# KCMUtils requires an ungenerated <target>.json in this source directory to
# create the System Settings desktop entry. Keep that file version-neutral,
# then add the canonical version to the copy embedded in the plugin. This
# preserves VERSION as the only hand-edited release value while satisfying
# KCMUtils' source-file convention.
set(_nasmount_kcm_metadata_source "${CMAKE_CURRENT_SOURCE_DIR}/kcm_nasmount.json")
file(READ "${_nasmount_kcm_metadata_source}" _nasmount_kcm_metadata)
string(JSON _nasmount_kcm_plugin_metadata
       SET "${_nasmount_kcm_metadata}" KPlugin Version "\"${PROJECT_VERSION}\"")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/kcm_nasmount_plugin.json"
     CONTENT "${_nasmount_kcm_plugin_metadata}\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${_nasmount_kcm_metadata_source}")

kcmutils_add_qml_kcm(kcm_nasmount
    SOURCES
        kcmnasmount.cpp
        kcmnasmount.h
)
target_include_directories(kcm_nasmount PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kcm_nasmount PRIVATE
    kde_nasmount-core
    kde_nasmount-session
    Qt6::Core
    Qt6::DBus
    Qt6::Quick
    KF6::KCMUtilsQuick
    KF6::CoreAddons
)
