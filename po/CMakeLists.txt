# Message catalogs. Missing gettext tools are not an error: the build succeeds
# and the binaries run in English.
find_program(MSGFMT_EXECUTABLE msgfmt)
find_program(XGETTEXT_EXECUTABLE xgettext)

if(NOT MSGFMT_EXECUTABLE)
    message(STATUS "msgfmt not found; translations will not be built")
    return()
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/LINGUAS" TETHER_LINGUAS)

set(GMO_FILES)
foreach(LANG ${TETHER_LINGUAS})
    set(PO "${CMAKE_CURRENT_SOURCE_DIR}/${LANG}.po")
    if(NOT EXISTS "${PO}")
        continue()
    endif()

    # Built into the tree so an uninstalled run works:
    #   TETHER_LOCALEDIR=$PWD/build/debug/locale ./build/debug/tether-gtk
    set(GMO "${CMAKE_BINARY_DIR}/locale/${LANG}/LC_MESSAGES/tether.mo")
    add_custom_command(
        OUTPUT "${GMO}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/locale/${LANG}/LC_MESSAGES"
        COMMAND ${MSGFMT_EXECUTABLE} --check-format -o "${GMO}" "${PO}"
        DEPENDS "${PO}"
        COMMENT "Compiling ${LANG}.po"
        VERBATIM)
    list(APPEND GMO_FILES "${GMO}")

    install(FILES "${GMO}"
            DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${LANG}/LC_MESSAGES")
endforeach()

add_custom_target(translations ALL DEPENDS ${GMO_FILES})

if(XGETTEXT_EXECUTABLE)
    add_custom_target(pot
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/update-pot.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Regenerating po/tether.pot"
        VERBATIM)
endif()
