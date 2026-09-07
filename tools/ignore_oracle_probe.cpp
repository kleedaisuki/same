/** @file
 * @brief Git 忽略规则差分测试桥接程序。 / Bridge for differential Git ignore testing.
 */
#include "same/config.hpp"
#include <iostream>

/** 从标准输入读取相对路径；末尾 / 表示目录，输出每行 0 或 1。
 * Read relative paths from stdin; a trailing / denotes a directory; emit 0 or 1 per line.
 */
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    same::Ignore rules(argv[1]);
    std::string line;
    while (std::getline(std::cin, line)) {
        const bool directory = !line.empty() && line.back() == '/';
        if (directory)
            line.pop_back();
        std::cout << rules.matches(line, directory) << '\n';
    }
}
