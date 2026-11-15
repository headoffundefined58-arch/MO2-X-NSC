// 通过 MSVC /FI 强制包含，给「极简 uibase 构建」(versioninfo.cpp) 补上它依赖、但平时由
// uibase 的 pch.h 提供的标准库头。不要修改 uibase 子模块本身。
//
// 注意：versioninfo.cpp 里实际用到了 QLocale / QDate（如 displayString() 里的
// QLocale::system().toString(QDate(...))），但 uibase 平时靠 pch.h 间接把它们带进来。
// 在只编 versioninfo.cpp 的极简构建里必须显式包含，否则 MSVC 会报“未声明的标识符”。
#include <map>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <QLocale>
#include <QDate>
