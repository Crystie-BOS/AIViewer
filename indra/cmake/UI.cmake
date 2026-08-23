# -*- cmake -*-
include(Prebuilt)
include(FreeType)

add_library( ll::uilibraries INTERFACE IMPORTED )

if (LINUX)
  target_compile_definitions(ll::uilibraries INTERFACE LL_GTK=1 LL_X11=1 )

  if( USE_CONAN )
    target_link_libraries( ll::uilibraries INTERFACE CONAN_PKG::gtk )
    return()
  else()
    set(LOCAL_DEV_ROOT "/home/crystie/AIViewer/local-dev-root" CACHE PATH
        "Root of the locally extracted 64-bit Linux development packages")
    set(_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
    set(_saved_pkg_config_sysroot "$ENV{PKG_CONFIG_SYSROOT_DIR}")
    set(ENV{PKG_CONFIG_PATH}
        "${LOCAL_DEV_ROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${LOCAL_DEV_ROOT}/usr/share/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${LOCAL_DEV_ROOT}")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK2 REQUIRED IMPORTED_TARGET gtk+-2.0)

    set(ENV{PKG_CONFIG_PATH} "${_saved_pkg_config_path}")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_saved_pkg_config_sysroot}")
    target_link_libraries(ll::uilibraries INTERFACE PkgConfig::GTK2 ll::freetype)
  endif()
endif (LINUX)
if( WINDOWS )
  target_link_libraries( ll::uilibraries INTERFACE
          opengl32
          comdlg32
          dxguid
          kernel32
          odbc32
          odbccp32
          oleaut32
          shell32
          Vfw32
          wer
          winspool
          imm32
          )
endif()

target_include_directories( ll::uilibraries SYSTEM INTERFACE
        ${LIBS_PREBUILT_DIR}/include
        )
