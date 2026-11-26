# 地图质量检查工具（map_quality_check_tools）

用于地图数据质量检查的 C++ / Qt / QGIS 插件源码。

## 主要内容

- `algorithm/`：要素几何、拓扑、图幅接边、跨比例尺及其他质检算法。
- `ui/`、`ui_class/`、`ui_task/`：质检界面、交互和任务管理。
- `commontype/`：公共数据类型。
- `icon/`、`images/`：界面资源。
- `tests/`、`docs/` 及根目录说明文件：测试与实现说明。
- `map_quality_check_tools/`：原目录自带的另一份工程，按原结构保留；默认入口为根目录解决方案。

## 构建

1. 安装 Visual Studio C++ 工具链、Qt 及对应版本的 QGIS/GDAL 开发依赖。
2. 打开根目录 `map_quality_check_tools.sln`。
3. 检查 `.vcxproj` 的 Qt、头文件、库及输出路径，选择匹配的 x64 配置构建。
4. 将生成的插件部署到匹配的 LTZK/QGIS 运行环境进行验证。

## 源码来源

- 本机目录：`E:\map_quality_check_tools`
- 配套平台：[LTZK](https://github.com/7k7kiyomi/LTZK)

## 版本说明

本仓库为 2026-11-26 整理的源码快照。保留现有工程结构、源码、界面资源、测试和开发文档；未包含编译产物、调试符号、历史备份、临时构建目录、数据库口令及第三方运行环境。原目录中的文件未被修改。

工程中的 Qt、QGIS、GDAL 等依赖路径来自原开发机器，首次构建时需按本机安装位置调整。此次归档进行了文件清单、敏感配置及资源引用检查，未执行完整编译或运行验证。
