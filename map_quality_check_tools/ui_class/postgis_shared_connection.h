#pragma once

#include <QString>

// 数据库连接和数据预处理在同一线程中复用这一条应用级 PostGIS 连接。
// 连接由 Qt 的连接池在应用退出时统一释放，任一对话框关闭都不会中断另一个入口。
inline QString mapProductManagerSharedPostgisConnectionName()
{
	return QStringLiteral("MapProductManagerSharedPostgis");
}
