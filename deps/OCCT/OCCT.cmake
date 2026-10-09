if(WIN32)
    set(library_build_type "Shared")
else()
    set(library_build_type "Static")
endif()

if (IN_GIT_REPO)
    set(OCCT_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_OCCT-prefix/src/dep_OCCT)
endif ()

orcaslicer_add_cmake_project(OCCT
    URL https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V7_6_0.zip
    URL_HASH SHA256=28334f0e98f1b1629799783e9b4d21e05349d89e695809d7e6dfa45ea43e1dbc
    #PATCH_COMMAND ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-OCCT-fix.patch
    PATCH_COMMAND git apply ${OCCT_DIRECTORY_FLAG} --verbose --ignore-space-change --whitespace=fix ${CMAKE_CURRENT_LIST_DIR}/0001-OCCT-fix.patch
    #DEPENDS dep_Boost
    DEPENDS ${FREETYPE_PKG}
    CMAKE_ARGS
        -DBUILD_LIBRARY_TYPE=${library_build_type}
        # Protype: the C++ standard is stated, not left to the compiler.
        # OCCT 7.6 adds -std=c++0x itself only when CMAKE_CXX_COMPILER_ID is
        # exactly "Clang". Under policy CMP0025 - always on in CMake 4, and
        # on in older CMake once CMAKE_POLICY_VERSION_MINIMUM=3.5 is set, as
        # the daemon's build script does - Apple's compiler reports
        # "AppleClang", the flag is skipped and OCCT builds in the compiler's
        # default mode. Apple clang 21 defaults to gnu++14 and gets away with
        # it; AppleClang 15 (Xcode 15.4) defaults to gnu++98 and dies on
        # `handle<BVH_Tree<T, N>>&` in BVH_Traverse.hxx. 14 is what the
        # workstation build has been compiling OCCT with all along.
        -DCMAKE_CXX_STANDARD=14
        -DUSE_TK=OFF
        -DUSE_TBB=OFF
	#-DUSE_FREETYPE=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_VTK=OFF
        -DBUILD_DOC_Overview=OFF
        -DBUILD_MODULE_ApplicationFramework=OFF
        #-DBUILD_MODULE_DataExchange=OFF
        -DBUILD_MODULE_Draw=OFF
        -DBUILD_MODULE_FoundationClasses=OFF
        # Protype: ON, not OFF. Reading STEP needs none of this module, so
        # upstream leaves it out - but our FFI bridge calls BRepFilletAPI
        # (fillets/chamfers) and BRepOffsetAPI, whose toolkits TKFillet and
        # TKOffset only get built when the module is on. With it off the
        # Windows deps prefix ships 30 TK libs and no BRepFilletAPI headers,
        # and the daemon dies at compile time:
        #   fatal error C1083: cannot open include file BRepFilletAPI_MakeFillet.hxx
        -DBUILD_MODULE_ModelingAlgorithms=ON
        -DBUILD_MODULE_ModelingData=OFF
        -DBUILD_MODULE_Visualization=OFF
)

# add_dependencies(dep_OCCT ${FREETYPE_PKG})
