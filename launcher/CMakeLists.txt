# C64 launcher (cc65): build/GOCartLauncher.bin in the build directory,
# appended to the firmware by the main CMakeLists.txt.
#
# Needs cc65 (cc65, ca65, ar65, ld65). cc65 is searched in the PATH, or set
# CC65_BIN_DIR. For a cc65 that is not installed (built from source), the
# directory with its include/, asminc/ and lib/ is found from the binaries
# (or set CC65_HOME).
#
# Sets GOCART_LAUNCHER_BIN (path of the launcher image) and the target
# "launcher" in the parent scope.

set(GOCART_LAUNCHER_VERSION "1.0" CACHE STRING "Launcher version shown on the C64 screen")
set(GOCART_LAUNCHER_THEME "1" CACHE STRING "Launcher color theme (see launcher/screen.h)")
option(GOCART_LAUNCHER_THIN_FONT "Launcher font with 1 pixel wide strokes (see launcher/screen.c)" OFF)
if(GOCART_LAUNCHER_THIN_FONT)
   set(LAUNCHER_THIN_FONT 1)
else()
   set(LAUNCHER_THIN_FONT 0)
endif()
set(CC65_BIN_DIR "" CACHE PATH "Directory of the cc65 binaries (empty: PATH)")
set(CC65_HOME "$ENV{CC65_HOME}" CACHE PATH "cc65 directory with include/, asminc/, lib/ (empty: installed cc65)")

set(LAUNCHER_SRC ${CMAKE_CURRENT_SOURCE_DIR})
set(LAUNCHER_OUT ${CMAKE_CURRENT_BINARY_DIR})
set(LAUNCHER_BIN ${LAUNCHER_OUT}/GOCartLauncher.bin)

if(CC65_BIN_DIR)
   set(CC65_HINTS HINTS ${CC65_BIN_DIR} NO_DEFAULT_PATH)
endif()
find_program(CC65_CC65 cc65 ${CC65_HINTS})
find_program(CC65_CA65 ca65 ${CC65_HINTS})
find_program(CC65_AR65 ar65 ${CC65_HINTS})
find_program(CC65_LD65 ld65 ${CC65_HINTS})

if(NOT CC65_CC65 OR NOT CC65_CA65 OR NOT CC65_AR65 OR NOT CC65_LD65)
   message(FATAL_ERROR "cc65 not found (cc65, ca65, ar65, ld65): set CC65_BIN_DIR")
endif()

# cc65 not installed (built from source): its files next to the binaries
get_filename_component(CC65_BIN_PATH ${CC65_CC65} DIRECTORY)
if(NOT CC65_HOME AND EXISTS ${CC65_BIN_PATH}/../include/conio.h)
   get_filename_component(CC65_HOME ${CC65_BIN_PATH}/.. ABSOLUTE)
endif()
if(CC65_HOME)
   set(CC65_ENV ${CMAKE_COMMAND} -E env CC65_HOME=${CC65_HOME})
else()
   set(CC65_ENV "")
endif()

# C64 runtime library
# (not find_file: the Pico SDK toolchain file limits it to the ARM sysroot)
set(CC65_C64_LIB "")
foreach(d ${CC65_HOME}/lib /usr/share/cc65/lib /usr/local/share/cc65/lib
          /usr/lib/cc65/lib /usr/local/lib/cc65/lib)
   if(NOT CC65_C64_LIB AND EXISTS ${d}/c64.lib)
      set(CC65_C64_LIB ${d}/c64.lib)
   endif()
endforeach()
if(NOT CC65_C64_LIB)
   message(FATAL_ERROR "cc65 c64.lib not found (set CC65_HOME)")
endif()

message(STATUS "Launcher: cc65 ${CC65_CC65}, ${CC65_C64_LIB}, version ${GOCART_LAUNCHER_VERSION}, theme ${GOCART_LAUNCHER_THEME}, thin font ${GOCART_LAUNCHER_THIN_FONT}")

# every C file depends on every header
file(GLOB LAUNCHER_HEADERS ${LAUNCHER_SRC}/*.h)
set(LAUNCHER_INC -I ${LAUNCHER_SRC} -I ${LAUNCHER_SRC}/libef3usb/src)

#
# libef3usb
#
set(EF3USB_SRC ${LAUNCHER_SRC}/libef3usb/src)
set(EF3USB_NAMES
   ef3usb_check_cmd ef3usb_discard_rx ef3usb_send_data ef3usb_send_str
   ef3usb_fread ef3usb_fload ef3usb_fclose
)
set(EF3USB_OBJS "")
foreach(n ${EF3USB_NAMES})
   set(o ${LAUNCHER_OUT}/libef3usb/${n}.o)
   add_custom_command(
      OUTPUT ${o}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${LAUNCHER_OUT}/libef3usb
      COMMAND ${CC65_ENV} ${CC65_CA65} -I ${EF3USB_SRC} -t c64 -o ${o} ${EF3USB_SRC}/${n}.s
      DEPENDS ${EF3USB_SRC}/${n}.s ${EF3USB_SRC}/ef3usb_macros.s
      WORKING_DIRECTORY ${LAUNCHER_SRC}/libef3usb
      VERBATIM
   )
   list(APPEND EF3USB_OBJS ${o})
endforeach()

set(EF3USB_LIB ${LAUNCHER_OUT}/libef3usb.lib)
add_custom_command(
   OUTPUT ${EF3USB_LIB}
   COMMAND ${CMAKE_COMMAND} -E rm -f ${EF3USB_LIB}
   COMMAND ${CC65_ENV} ${CC65_AR65} a ${EF3USB_LIB} ${EF3USB_OBJS}
   DEPENDS ${EF3USB_OBJS}
   VERBATIM
)

#
# launcher objects
#
set(LAUNCHER_C main base screen dir kff_data)
set(LAUNCHER_S crt0.crt disk ef3usb_loader launcher_asm)
set(LAUNCHER_ASM_DEPS
   ${LAUNCHER_SRC}/eapi-kung_fu_flash.s
   ${EF3USB_SRC}/ef3usb_macros.s
)

set(LAUNCHER_OBJS "")
foreach(n ${LAUNCHER_C})
   set(s ${LAUNCHER_OUT}/${n}.s)
   set(o ${LAUNCHER_OUT}/${n}.o)
   add_custom_command(
      OUTPUT ${s}
      COMMAND ${CC65_ENV} ${CC65_CC65} -t c64 -T -O --static-locals -g ${LAUNCHER_INC}
              -DGOCART_VER=\"${GOCART_LAUNCHER_VERSION}\" -DLAUNCHER_THEME=${GOCART_LAUNCHER_THEME}
              -DLAUNCHER_THIN_FONT=${LAUNCHER_THIN_FONT}
              -o ${s} ${LAUNCHER_SRC}/${n}.c
      DEPENDS ${LAUNCHER_SRC}/${n}.c ${LAUNCHER_HEADERS}
      WORKING_DIRECTORY ${LAUNCHER_SRC}
      VERBATIM
   )
   add_custom_command(
      OUTPUT ${o}
      COMMAND ${CC65_ENV} ${CC65_CA65} -t c64 -g ${LAUNCHER_INC} -o ${o} ${s}
      DEPENDS ${s}
      WORKING_DIRECTORY ${LAUNCHER_SRC}
      VERBATIM
   )
   list(APPEND LAUNCHER_OBJS ${o})
endforeach()

foreach(n ${LAUNCHER_S})
   set(o ${LAUNCHER_OUT}/${n}.o)
   add_custom_command(
      OUTPUT ${o}
      COMMAND ${CC65_ENV} ${CC65_CA65} -t c64 -g ${LAUNCHER_INC} -o ${o} ${LAUNCHER_SRC}/${n}.s
      DEPENDS ${LAUNCHER_SRC}/${n}.s ${LAUNCHER_ASM_DEPS} ${LAUNCHER_HEADERS}
      WORKING_DIRECTORY ${LAUNCHER_SRC}
      VERBATIM
   )
   list(APPEND LAUNCHER_OBJS ${o})
endforeach()

#
# link: crt0.crt.o first
#
list(REMOVE_ITEM LAUNCHER_OBJS ${LAUNCHER_OUT}/crt0.crt.o)
add_custom_command(
   OUTPUT ${LAUNCHER_BIN}
   COMMAND ${CC65_ENV} ${CC65_LD65} -o ${LAUNCHER_BIN} -Ln ${LAUNCHER_BIN}.lbl -m ${LAUNCHER_BIN}.map
           -C ${LAUNCHER_SRC}/ld.crt.cfg ${LAUNCHER_OUT}/crt0.crt.o ${LAUNCHER_OBJS}
           --lib ${EF3USB_LIB} --lib ${CC65_C64_LIB}
   DEPENDS ${LAUNCHER_OUT}/crt0.crt.o ${LAUNCHER_OBJS} ${EF3USB_LIB} ${LAUNCHER_SRC}/ld.crt.cfg
   VERBATIM
)

add_custom_target(launcher DEPENDS ${LAUNCHER_BIN})
set(GOCART_LAUNCHER_BIN ${LAUNCHER_BIN} PARENT_SCOPE)
