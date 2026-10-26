cmake_minimum_required(VERSION 3.21)

project(librepods-windows VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Widgets Bluetooth Network Concurrent LinguistTools)

qt_standard_project_setup()

# Windows has no user-mode L2CAP (neither Winsock AF_BTH nor QtBluetooth can
# open the AACP channel). The preferred transport is the LibrePods KMDF
# profile driver under windows/driver/kmdf (DriverL2CapSocket); the Winsock
# socket remains as a fallback so the app still functions partially without it.

set(LINUX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shared)

# Translation files (shared with the Linux build)
set(TS_FILES
    ${LINUX_DIR}/translations/librepods_tr.ts
)

qt_add_executable(librepods
    main.cpp
    BluetoothMonitor.cpp
    BluetoothMonitor.h
    autostartmanager.hpp
    systemsleepmonitor.hpp
    ${LINUX_DIR}/btsocket.h
    l2capsocket.cpp
    l2capsocket.h
    driverl2capsocket.cpp
    driverl2capsocket.h
    svcchannelsocket.cpp
    svcchannelsocket.h
    svcprotocol.h
    driver/shared/librepods_driver_protocol.h
    ble/blemanager.cpp
    ble/blemanager.h
    media/mediacontroller.cpp
    media/mediacontroller.h
    # Shared portable sources compiled straight from the Linux tree
    ${LINUX_DIR}/attmanager.cpp
    ${LINUX_DIR}/attmanager.h
    ${LINUX_DIR}/audiospectrum.cpp
    ${LINUX_DIR}/audiospectrum.h
    ${LINUX_DIR}/hearingaidmodel.hpp
    ${LINUX_DIR}/gesturedetector.h
    ${LINUX_DIR}/headorientation.h
    ${LINUX_DIR}/airpods_packets.h
    ${LINUX_DIR}/enums.h
    ${LINUX_DIR}/battery.hpp
    ${LINUX_DIR}/eardetection.hpp
    ${LINUX_DIR}/deviceinfo.hpp
    crashdump.cpp
    crashdump.h
    ${LINUX_DIR}/logger.h
    ${LINUX_DIR}/BasicControlCommand.hpp
    ${LINUX_DIR}/trayiconmanager.cpp
    ${LINUX_DIR}/trayiconmanager.h
    ${LINUX_DIR}/QRCodeImageProvider.hpp
    ${LINUX_DIR}/ble/bleinfo.h
    ${LINUX_DIR}/ble/proximityparser.cpp
    ${LINUX_DIR}/ble/proximityparser.h
    ${LINUX_DIR}/ble/bleutils.cpp
    ${LINUX_DIR}/ble/bleutils.h
    ${LINUX_DIR}/thirdparty/QR-Code-generator/qrcodegen.cpp
    ${LINUX_DIR}/thirdparty/QR-Code-generator/qrcodegen.hpp
    app.rc
)

set_target_properties(librepods PROPERTIES WIN32_EXECUTABLE TRUE)

# Files live in ../linux, outside this source dir, so each needs an
# explicit QT_RESOURCE_ALIAS for the resource system.
set(QML_FILES
    ${LINUX_DIR}/Main.qml
    ${LINUX_DIR}/BatteryIndicator.qml
    ${LINUX_DIR}/SegmentedControl.qml
    ${LINUX_DIR}/Icon.qml
    ${LINUX_DIR}/KeysQRDialog.qml
    ${LINUX_DIR}/HeadModel.qml
    ${LINUX_DIR}/StyledSwitch.qml
    ${LINUX_DIR}/StyledComboBox.qml
    ${LINUX_DIR}/StyledSlider.qml
    ${LINUX_DIR}/StyledButton.qml
    ${LINUX_DIR}/StyledTextField.qml
    ${LINUX_DIR}/StyledSpinBox.qml
)
set(RESOURCE_FILES
    ${LINUX_DIR}/assets/airpods.png
    ${LINUX_DIR}/assets/pod.png
    ${LINUX_DIR}/assets/pod_case.png
    ${LINUX_DIR}/assets/pod3.png
    ${LINUX_DIR}/assets/pod3_case.png
    ${LINUX_DIR}/assets/pod4_case.png
    ${LINUX_DIR}/assets/podpro.png
    ${LINUX_DIR}/assets/podpro_case.png
    ${LINUX_DIR}/assets/podmax.png
    ${LINUX_DIR}/assets/fonts/SF-Symbols-6.ttf
)
# QML files are referenced by basename within the module; assets must keep
# their assets/ relative paths (QML loads qrc:/icons/assets/...).
foreach(_f IN LISTS QML_FILES)
    get_filename_component(_name "${_f}" NAME)
    set_source_files_properties("${_f}" PROPERTIES QT_RESOURCE_ALIAS "${_name}")
endforeach()
foreach(_f IN LISTS RESOURCE_FILES)
    file(RELATIVE_PATH _name "${LINUX_DIR}" "${_f}")
    set_source_files_properties("${_f}" PROPERTIES QT_RESOURCE_ALIAS "${_name}")
endforeach()

qt_add_qml_module(librepods
    URI librepods
    VERSION 1.0
    # Sources live outside this dir; qmlcachegen generates invalid output
    # paths for those, so compile QML at runtime instead.
    NO_CACHEGEN
    QML_FILES ${QML_FILES}
)

qt_add_resources(librepods "resources"
    PREFIX "/icons"
    FILES ${RESOURCE_FILES}
)

# shims/ supplies <openssl/aes.h> (a bcrypt-backed stand-in); ../linux supplies
# the shared headers.
target_include_directories(librepods PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/shims
    ${LINUX_DIR}
    ${LINUX_DIR}/ble
    ${LINUX_DIR}/thirdparty/QR-Code-generator
)

target_link_libraries(librepods PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Bluetooth
    Qt6::Network
    Qt6::Concurrent
    ws2_32
    bthprops
    windowsapp
    bcrypt
    ole32
    rpcrt4
    setupapi
    dbghelp
)

# llvm-mingw does not ship the C++/WinRT projection or the WindowsApp
# umbrella import library; pull both from the newest installed Windows SDK.
if(MINGW AND NOT MSVC)
    file(GLOB _winsdk_includes LIST_DIRECTORIES true
        "C:/Program Files (x86)/Windows Kits/10/Include/*")
    if(NOT _winsdk_includes)
        message(FATAL_ERROR "Windows 10 SDK not found (needed for C++/WinRT headers)")
    endif()
    list(SORT _winsdk_includes ORDER DESCENDING)
    list(GET _winsdk_includes 0 _winsdk_include)
    get_filename_component(_winsdk_ver "${_winsdk_include}" NAME)
    # The SDK-bundled C++/WinRT projection is too old for C++20 toolchains;
    # prefer a freshly generated one (cppwinrt.exe -input ... -output ...).
    set(LIBREPODS_CPPWINRT_DIR "C:/Qt/Tools/cppwinrt-proj" CACHE PATH
        "Directory containing generated C++/WinRT headers (winrt/base.h)")
    if(EXISTS "${LIBREPODS_CPPWINRT_DIR}/winrt/base.h")
        set(_cppwinrt_inc "${LIBREPODS_CPPWINRT_DIR}")
    else()
        set(_cppwinrt_inc "${_winsdk_include}/cppwinrt")
    endif()
    target_include_directories(librepods SYSTEM PRIVATE
        "${_cppwinrt_inc}"
        "${_winsdk_include}/winrt"
    )
    target_link_directories(librepods PRIVATE
        "C:/Program Files (x86)/Windows Kits/10/Lib/${_winsdk_ver}/um/x64")
endif()

qt_add_translations(librepods
    TS_FILES ${TS_FILES}
    QM_FILES_OUTPUT_VARIABLE QM_FILES
)

install(TARGETS librepods
    RUNTIME DESTINATION .
)
install(FILES ${QM_FILES}
    DESTINATION "translations")

# Minimal CLI companion that sends commands to the running app.
add_executable(librepods-ctl
    ${LINUX_DIR}/librepods-ctl.cpp
)
set_target_properties(librepods-ctl PROPERTIES AUTOMOC OFF)
target_link_libraries(librepods-ctl PRIVATE Qt6::Core Qt6::Network)
install(TARGETS librepods-ctl RUNTIME DESTINATION .)

# Headless channel-owner service. Holds the driver handle + L2CAP sessions so
# a GUI crash/restart doesn't tear down the AirPods control channel — a
# reconnecting client adopts the live channel instead. Talks the framed
# protocol in svcprotocol.h over a named local socket.
add_executable(librepods-svc
    svc/librepods_svc.cpp
    driverl2capsocket.cpp
    driverl2capsocket.h
    svcprotocol.h
    ${LINUX_DIR}/btsocket.h
    driver/shared/librepods_driver_protocol.h
)
target_include_directories(librepods-svc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LINUX_DIR}
)
target_link_libraries(librepods-svc PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Bluetooth
    setupapi
)
install(TARGETS librepods-svc RUNTIME DESTINATION .)
