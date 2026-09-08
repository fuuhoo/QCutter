# 如果源码 SRC 目录存在且非空, 拷贝到 DST. 否则静默跳过.
# 用途: assets/maplibre/{*.js,*.css} 在 .gitignore 中, 仓库 clone 后 SRC 可能缺失.
if(EXISTS "${SRC}" AND IS_DIRECTORY "${SRC}")
    file(GLOB _files LIST_DIRECTORIES false "${SRC}/*")
    if(_files)
        file(COPY "${SRC}/" DESTINATION "${DST}")
        message(STATUS "Copied ${SRC} -> ${DST}")
    else()
        message(STATUS "Skipped (empty): ${SRC}")
    endif()
else()
    message(STATUS "Skipped (missing): ${SRC}")
endif()