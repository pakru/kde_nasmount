cmake_minimum_required(VERSION 3.20)

# VERSION is the sole release-version source. Keeping it independent of the
# build language lets packaging and release automation read the same value
# without parsing CMake syntax.
set(NASMOUNT_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${NASMOUNT_VERSION_FILE}" NASMOUNT_VERSION)
string(STRIP "${NASMOUNT_VERSION}" NASMOUNT_VERSION)
if(NOT NASMOUNT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "VERSION must contain exactly one MAJOR.MINOR.PATCH version; got "
        "'${NASMOUNT_VERSION}'.")
endif()

project(kde_nasmount VERSION "${NASMOUNT_VERSION}" LANGUAGES CXX)

# `source` preserves the checkout-driven install/uninstall lifecycle. Native
# packages select their family explicitly so the installed uninstaller never
# guesses which package manager owns the payload.
set(NASMOUNT_PACKAGE_FAMILY "source" CACHE STRING "Installation owner: source, deb, or rpm")
set_property(CACHE NASMOUNT_PACKAGE_FAMILY PROPERTY STRINGS source deb rpm)
if(NOT NASMOUNT_PACKAGE_FAMILY MATCHES "^(source|deb|rpm)$")
    message(FATAL_ERROR
        "NASMOUNT_PACKAGE_FAMILY must be source, deb, or rpm; got "
        "'${NASMOUNT_PACKAGE_FAMILY}'.")
endif()

# A normal `cmake --build` must notice a version bump even when the user does
# not explicitly run the configure command first.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${NASMOUNT_VERSION_FILE}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# kde_nasmount-core/-session are STATIC (deliberately: see below) but kcm_nasmount
# links them into a plugin (a shared object), which requires PIC code.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Concurrent Quick QuickControls2)
# KIO is required for KPasswdServerClient alone, the public way to ask the
# session whether it knows a password for a share (autofill plan §2.2). It is
# linked into nasmount-dialog only, never into core and so never into the
# privileged helper.
find_package(KF6 6.0 REQUIRED COMPONENTS Auth I18n WidgetsAddons Config CoreAddons KCMUtils KIO)

set(CMAKE_AUTOMOC ON)

set(NASMOUNT_HELPER_ID "io.github.pakru.nasmount")

configure_file(src/core/nasmountversion.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/nasmountversion.h
               @ONLY)

# ---------------------------------------------------------------------------
# core — validation, unit-value encoding, and the read-only state model
# (docs/credential-modes-design.md §§4-6). Linked into every binary,
# including the privileged helper, which is exactly why it stays a STATIC
# library rather than a shared one: the helper must have no runtime
# dependency on a .so a non-root user could replace.
# ---------------------------------------------------------------------------
add_library(kde_nasmount-core STATIC
    src/core/packagestate.cpp
    src/core/unitspec.cpp
    src/core/unitvalue.cpp
    src/core/verify.cpp
)
target_include_directories(kde_nasmount-core PUBLIC
    src/core
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(kde_nasmount-core PUBLIC Qt6::Core)

# ---------------------------------------------------------------------------
# session — Store (KConfig), the per-user lock, the KAuth call
# wrapper, the async operation controller, and the display model. Never
# linked into the helper (plan §8: "session -> dialog, kcm --
# never the helper").
# ---------------------------------------------------------------------------
add_library(kde_nasmount-session STATIC
    src/session/cleanupvalidation.cpp
    src/session/store.cpp
    src/session/userlock.cpp
    src/session/helperinvoke.cpp
    src/session/mountactions.cpp
    src/session/mountmodel.cpp
)
target_include_directories(kde_nasmount-session PUBLIC src/session)
target_link_libraries(kde_nasmount-session PUBLIC
    kde_nasmount-core
    Qt6::Core Qt6::Concurrent
    KF6::AuthCore KF6::ConfigCore
)

# ---------------------------------------------------------------------------
# root — the private, privileged library: descriptor-based durable filesystem
# primitives, the root lock, systemd command execution, and credential/
# runtime artifact stores (plan §2.1). Linked ONLY into privileged
# executables (nasmount-helper, nasmount-boot). NEVER linked into
# kde_nasmount-session, the KCM, or the dialog — those are
# reachable by an unprivileged, potentially hostile local process, and must
# have no code path that can mutate root-owned state. Enforced below by
# kde_nasmount_assert_no_root_link(), not just by this comment.
# ---------------------------------------------------------------------------
add_library(kde_nasmount-root STATIC
    src/root/arming.cpp
    src/root/credentialstore.cpp
    src/root/durablefs.cpp
    src/root/inventory.cpp
    src/root/operations.cpp
    src/root/rootlock.cpp
    src/root/runtimefiles.cpp
    src/root/systemdops.cpp
)
target_include_directories(kde_nasmount-root PUBLIC src/root)
target_link_libraries(kde_nasmount-root PUBLIC kde_nasmount-core Qt6::Core)

# --- privileged KAuth helper ------------------------------------------------
add_executable(nasmount-helper src/helper/helper.cpp)
# No Widgets, no I18n, no session library: the root helper produces no UI, no
# translated strings, and must not link anything that talks to KConfig
# on the caller's behalf. libQt6Gui still appears in ldd output as a transitive
# dependency of KF6CoreAddons, which KAuth requires — but nothing here
# constructs a QGuiApplication, so no GUI machinery is initialised.
target_link_libraries(nasmount-helper PRIVATE kde_nasmount-core kde_nasmount-root Qt6::Core KF6::AuthCore)
install(TARGETS nasmount-helper DESTINATION ${KAUTH_HELPER_INSTALL_DIR})

# Generates and installs the D-Bus policy + system service file that let the
# helper own its name on the system bus and be activated on demand.
kauth_install_helper_files(nasmount-helper ${NASMOUNT_HELPER_ID} root)

# Turns the .actions file into a polkit .policy via kauth-policy-gen.
kauth_install_actions(${NASMOUNT_HELPER_ID} ${CMAKE_CURRENT_SOURCE_DIR}/${NASMOUNT_HELPER_ID}.actions)

# --- privileged boot coordinator (phase 4) ----------------------------------
# Started directly by systemd as root (nasmount-boot.service) -- never D-Bus/
# KAuth activated, so no .actions entry and no polkit policy: there is no
# caller to authorize, only root arming triggers it already owns (design
# §6.3). Installed into the KDE libexec dir, matching the service template's
# ExecStart=.
add_executable(nasmount-boot src/boot/main.cpp)
target_link_libraries(nasmount-boot PRIVATE kde_nasmount-core kde_nasmount-root Qt6::Core)
install(TARGETS nasmount-boot DESTINATION ${KDE_INSTALL_LIBEXECDIR})

configure_file(systemd/nasmount-boot.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/nasmount-boot.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nasmount-boot.service
        DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/system)

# --- unprivileged GUI: Dolphin service-menu dialog --------------------------
# dialog.qrc embeds ShareForm.qml from src/kcm/ui -- one form definition
# serving both front ends, rather than a second copy that can drift.
qt_add_resources(NASMOUNT_DIALOG_QRC src/dialog/dialog.qrc)
# credentiallookupworker.cpp is a private mode of this same binary rather than
# an executable of its own, which leaves the installed file set -- and so the
# cleanup manifest and both package payload lists -- untouched (plan §6).
add_executable(nasmount-dialog
    src/dialog/main.cpp
    src/dialog/credentiallookup.cpp
    src/dialog/credentiallookupworker.cpp
    src/dialog/dialogbackend.cpp
    src/dialog/smburl.cpp
    ${NASMOUNT_DIALOG_QRC}
)
target_link_libraries(nasmount-dialog
    PRIVATE kde_nasmount-core kde_nasmount-session
            Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2
            KF6::I18n KF6::KIOCore
)
install(TARGETS nasmount-dialog DESTINATION ${KDE_INSTALL_BINDIR})

# --- authenticated uninstall coordinator -----------------------------------
# Runs as the user while the helper/policy still exist. It validates the
# install manifest, invokes the single auth_admin purge action, then removes
# config data only after the privileged purge is confirmed.
add_executable(nasmount-cleanup src/cleanup/main.cpp)
target_link_libraries(nasmount-cleanup PRIVATE kde_nasmount-session Qt6::Core)
install(TARGETS nasmount-cleanup DESTINATION ${KDE_INSTALL_BINDIR})

# --- native-package removal guard and persistent uninstall entry point ------
# The guard is intentionally linked only to read-only core code. Package
# maintainer scripts run it before erasure; it cannot mutate or repair state.
add_executable(nasmount-package-guard src/packageguard/main.cpp)
target_link_libraries(nasmount-package-guard PRIVATE kde_nasmount-core Qt6::Core)
install(TARGETS nasmount-package-guard DESTINATION ${KDE_INSTALL_LIBEXECDIR})

install(PROGRAMS packaging/nasmount-uninstall.sh
        DESTINATION ${KDE_INSTALL_BINDIR}
        RENAME nasmount-uninstall)

configure_file(packaging/package-family.in
               ${CMAKE_CURRENT_BINARY_DIR}/package-family
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/package-family
        DESTINATION ${KDE_INSTALL_DATADIR}/nasmount)
if(NASMOUNT_PACKAGE_FAMILY STREQUAL "rpm")
    set(NASMOUNT_LICENSE_INSTALL_DIR "${KDE_INSTALL_DATADIR}/licenses/nasmount")
else()
    set(NASMOUNT_LICENSE_INSTALL_DIR "${KDE_INSTALL_DATADIR}/doc/nasmount")
endif()
install(FILES LICENSE DESTINATION ${NASMOUNT_LICENSE_INSTALL_DIR})

if(NASMOUNT_PACKAGE_FAMILY STREQUAL "rpm")
    install(FILES packaging/rpm/90-nasmount.preset
            DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/system-preset)
endif()


# --- KCM: System Settings module --------------------------------------------
add_subdirectory(src/kcm)

# --- Dolphin service menu ---------------------------------------------------
# Exec= is substituted with the absolute install path rather than relying on
# PATH. Dolphin launches service-menu actions as transient systemd user units,
# whose PATH is the systemd user manager's — not a shell's.
configure_file(servicemenu/nasmount.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/nasmount.desktop @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nasmount.desktop
        DESTINATION ${KDE_INSTALL_DATADIR}/kio/servicemenus
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

# The authenticated cleanup client accepts only a complete, configure-time
# manifest. Generate it from the same KDEInstallDirs values used above so
# Debian multiarch and Fedora libexec/plugin layouts cannot drift apart.
function(nasmount_full_install_path output directory)
    if(IS_ABSOLUTE "${directory}")
        set(${output} "${directory}" PARENT_SCOPE)
    else()
        set(${output} "${CMAKE_INSTALL_PREFIX}/${directory}" PARENT_SCOPE)
    endif()
endfunction()

nasmount_full_install_path(NASMOUNT_FULL_BINDIR "${KDE_INSTALL_BINDIR}")
nasmount_full_install_path(NASMOUNT_FULL_LIBEXECDIR "${KDE_INSTALL_LIBEXECDIR}")
nasmount_full_install_path(NASMOUNT_FULL_PLUGINDIR "${KDE_INSTALL_PLUGINDIR}")
nasmount_full_install_path(NASMOUNT_FULL_SYSTEMDUNITDIR "${KDE_INSTALL_SYSTEMDUNITDIR}")
nasmount_full_install_path(NASMOUNT_FULL_DATADIR "${KDE_INSTALL_DATADIR}")
nasmount_full_install_path(NASMOUNT_FULL_KAUTH_HELPER_DIR "${KAUTH_HELPER_INSTALL_DIR}")

if(NASMOUNT_PACKAGE_FAMILY STREQUAL "rpm")
    set(NASMOUNT_LICENSE_MANIFEST_PATH "${NASMOUNT_FULL_DATADIR}/licenses/nasmount/LICENSE")
else()
    set(NASMOUNT_LICENSE_MANIFEST_PATH "${NASMOUNT_FULL_DATADIR}/doc/nasmount/LICENSE")
endif()

set(NASMOUNT_PRESET_MANIFEST_LINE "")
if(NASMOUNT_PACKAGE_FAMILY STREQUAL "rpm")
    set(NASMOUNT_PRESET_MANIFEST_LINE
        "${NASMOUNT_FULL_SYSTEMDUNITDIR}/system-preset/90-nasmount.preset")
endif()
configure_file(packaging/cleanup-manifest.in
               ${CMAKE_CURRENT_BINARY_DIR}/cleanup-manifest.txt
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cleanup-manifest.txt
        DESTINATION ${KDE_INSTALL_DATADIR}/nasmount)

# --- linkage invariant: kde_nasmount-root reaches only privileged executables ---
# (plan §2.1.2). Structural placement (never adding it to these
# target_link_libraries calls) is the primary defence; this is the check that
# catches a future accidental addition instead of relying on that forever.
function(kde_nasmount_assert_no_root_link target)
    get_target_property(_nasmount_libs ${target} LINK_LIBRARIES)
    if(_nasmount_libs AND "kde_nasmount-root" IN_LIST _nasmount_libs)
        message(FATAL_ERROR
            "${target} must never link kde_nasmount-root (plan 2.1.2): privileged "
            "filesystem/systemd mutation code must not be reachable from an "
            "unprivileged, potentially hostile local process.")
    endif()
endfunction()

kde_nasmount_assert_no_root_link(kde_nasmount-session)
kde_nasmount_assert_no_root_link(nasmount-dialog)
kde_nasmount_assert_no_root_link(nasmount-cleanup)
kde_nasmount_assert_no_root_link(nasmount-package-guard)
kde_nasmount_assert_no_root_link(kcm_nasmount)

# --- tests -------------------------------------------------------------------
# Not installed; built so `ctest` can exercise the same validation/encoding/
# verification code the privileged helper relies on.
enable_testing()

add_executable(unitspec_test tests/unitspec_test.cpp)
target_link_libraries(unitspec_test PRIVATE kde_nasmount-core)
add_test(NAME unitspec COMMAND unitspec_test)

# smburl links the dialog's own translation unit directly rather than a
# library: it is the only non-GUI part of nasmount-dialog, and pulling it into
# a library purely for the test would be more structure than it earns.
add_executable(smburl_test tests/smburl_test.cpp src/dialog/smburl.cpp)
target_include_directories(smburl_test PRIVATE src/dialog)
target_link_libraries(smburl_test PRIVATE kde_nasmount-core KF6::I18n)
add_test(NAME smburl COMMAND smburl_test)

# Links the dialog translation units the way smburl_test does, and
# deliberately not KF6::KIOCore: everything tested here is pure, so the suite
# never needs a wallet, a password service or a NAS.
add_executable(credentiallookup_test
    tests/credentiallookup_test.cpp
    src/dialog/credentiallookup.cpp
    src/dialog/smburl.cpp
)
target_include_directories(credentiallookup_test PRIVATE src/dialog)
target_link_libraries(credentiallookup_test PRIVATE kde_nasmount-core KF6::I18n)
add_test(NAME credentiallookup COMMAND credentiallookup_test)

# Loads the *real* ShareForm.qml for the one rule no C++ model can stand in
# for: that an imported credential is never paired with a typed one (plan
# §8.1). Read from the source tree, since a staged copy can go stale.
add_executable(shareform_qml_test tests/shareform_qml_test.cpp)
target_link_libraries(shareform_qml_test PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick)
target_compile_definitions(shareform_qml_test
    PRIVATE NASMOUNT_SHAREFORM_QML="${CMAKE_CURRENT_SOURCE_DIR}/src/kcm/ui/ShareForm.qml")
add_test(NAME shareform_qml COMMAND shareform_qml_test)

add_executable(unitvalue_test tests/unitvalue_test.cpp)
target_link_libraries(unitvalue_test PRIVATE kde_nasmount-core)
add_test(NAME unitvalue COMMAND unitvalue_test)

add_executable(verify_test tests/verify_test.cpp)
target_link_libraries(verify_test PRIVATE kde_nasmount-core)
add_test(NAME verify COMMAND verify_test)

add_executable(helperinvoke_test tests/helperinvoke_test.cpp)
target_link_libraries(helperinvoke_test PRIVATE kde_nasmount-session)
add_test(NAME helperinvoke COMMAND helperinvoke_test)

add_executable(store_test tests/store_test.cpp)
target_link_libraries(store_test PRIVATE kde_nasmount-session KF6::ConfigCore)
add_test(NAME store COMMAND store_test)

add_executable(mountactions_test tests/mountactions_test.cpp)
target_link_libraries(mountactions_test PRIVATE kde_nasmount-session)
add_test(NAME mountactions COMMAND mountactions_test)

add_executable(mountmodel_test tests/mountmodel_test.cpp)
target_link_libraries(mountmodel_test PRIVATE kde_nasmount-session)
add_test(NAME mountmodel COMMAND mountmodel_test)

add_executable(durablefs_test tests/durablefs_test.cpp)
target_link_libraries(durablefs_test PRIVATE kde_nasmount-root)
add_test(NAME durablefs COMMAND durablefs_test)

add_executable(inventory_test tests/inventory_test.cpp)
target_link_libraries(inventory_test PRIVATE kde_nasmount-root)
add_test(NAME inventory COMMAND inventory_test)

add_executable(operations_test tests/operations_test.cpp)
target_link_libraries(operations_test PRIVATE kde_nasmount-root)
add_test(NAME operations COMMAND operations_test)

add_executable(arming_test tests/arming_test.cpp)
target_link_libraries(arming_test PRIVATE kde_nasmount-root)
add_test(NAME arming COMMAND arming_test)

add_executable(credentialstore_test tests/credentialstore_test.cpp)
target_link_libraries(credentialstore_test PRIVATE kde_nasmount-root)
add_test(NAME credentialstore COMMAND credentialstore_test)

add_executable(cleanupvalidation_test tests/cleanupvalidation_test.cpp)
target_link_libraries(cleanupvalidation_test PRIVATE kde_nasmount-session)
add_test(NAME cleanupvalidation COMMAND cleanupvalidation_test)

add_executable(packagestate_test tests/packagestate_test.cpp)
target_link_libraries(packagestate_test PRIVATE kde_nasmount-core)
add_test(NAME packagestate COMMAND packagestate_test)

# The frozen unit corpus is read from the source tree, not staged into the
# build tree: it is a record of released bytes, and copying it would make a
# stale copy possible.
add_executable(goldenunits_test tests/goldenunits_test.cpp)
target_link_libraries(goldenunits_test PRIVATE kde_nasmount-core)
target_compile_definitions(goldenunits_test
    PRIVATE NASMOUNT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/units")
add_test(NAME goldenunits COMMAND goldenunits_test)

# The QML/C++ boundary is invisible to the compiler, qmllint and every ctest
# binary: QML resolves these names at runtime against a QObject exposed as
# `var`. This gate is the only thing that catches a C++ rename leaving a QML
# call site behind.
add_test(NAME qml_invokable_gate
         COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/qml_invokable_gate.sh)

add_test(NAME removed_api_gates
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/removed_api_gates.sh)
set_tests_properties(removed_api_gates PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME package_scripts
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_scripts_test.sh)
set_tests_properties(package_scripts PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME packaging_metadata
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/packaging_metadata_test.sh)
set_tests_properties(packaging_metadata PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Protect the release contract at generated and executable boundaries: VERSION,
# CMake's project version, product --version output, and KCM metadata agree.
add_test(NAME version_metadata
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FILE=${NASMOUNT_VERSION_FILE}
        -DGENERATED_HEADER=${CMAKE_CURRENT_BINARY_DIR}/generated/nasmountversion.h
        -DKCM_METADATA=${CMAKE_CURRENT_BINARY_DIR}/src/kcm/kcm_nasmount_plugin.json
        -DBOOT_EXECUTABLE=$<TARGET_FILE:nasmount-boot>
        -DCLEANUP_EXECUTABLE=$<TARGET_FILE:nasmount-cleanup>
        -DGUARD_EXECUTABLE=$<TARGET_FILE:nasmount-package-guard>
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/version_metadata_test.cmake
)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
