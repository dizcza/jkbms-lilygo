# Writes the build time to OUT as a C string; run on every build, see main/CMakeLists.txt.
string(TIMESTAMP now "%Y-%m-%d %H:%M")
set(content "const char *const build_time = \"${now}\";\n")
# rewrite only on change, so that an unchanged minute does not relink
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
    if(old STREQUAL content)
        return()
    endif()
endif()
file(WRITE "${OUT}" "${content}")
