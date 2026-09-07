# Optional developer targets; never require LLVM to build the application.
# 可选开发目标；编译程序不要求安装 LLVM。
find_program(SAME_CLANG_FORMAT NAMES clang-format clang-format-22 clang-format-21 clang-format-20)
if(SAME_CLANG_FORMAT)
    # This glob is only for formatting, never for defining compiled sources.
    # 此 glob 仅枚举格式化文件，不用于定义编译目标，避免触碰第三方依赖。
    file(GLOB_RECURSE same_format_sources CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/src/*.cu"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp")
    add_custom_target(format
        COMMAND "${SAME_CLANG_FORMAT}" -i --style=file ${same_format_sources}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Formatting project C++ and CUDA sources / 格式化项目源码"
        VERBATIM)
    add_custom_target(format-check
        COMMAND "${SAME_CLANG_FORMAT}" --dry-run --Werror --style=file ${same_format_sources}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Checking project formatting / 检查项目格式"
        VERBATIM)
endif()
