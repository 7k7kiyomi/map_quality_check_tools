# map_check_algorithms.cpp 修改说明

## 对比范围

- 当前文件：`E:/map_quality_check_tools/ui_class/map_check_algorithms.cpp`。
- 对比基准：`E:/map_quality_check_tools/tmp/road_topology_before_20261030/map_check_algorithms.cpp`，是本次道路拓扑修改前保留的源码备份。
- 工作目录不是 Git 仓库，本说明依据上述两份文件的文本差异，而非 Git 历史。
- 差异集中在 `checkLineTopology(...)`（线要素拓扑检查，ID 112）入口；本文件中仅有下述两处实质修改。

## 1. 找不到成果线图层时标记失败

位置：`checkLineTopology(...)` 中 `actualShpSemicolon.isEmpty()` 分支（当前文件约第 5480 行）。

- 修改前：未找到成果图层时返回 `status = "质检成功"`、`err_exist = false`，描述为“实际图层不存在”。
- 修改后：返回 `status = "质检失败"`、`err_exist = true`，`info.description` 和 `info.summary` 均说明“未执行：未找到匹配的成果线图层，不能视为拓扑检查通过”。
- 目的：区分未执行和检查通过，避免无输入时显示为成功。
- 边界：该分支检查传入的成果路径字符串是否为空；非空但文件不存在的情况由后续接口处理。

## 2. 校验所选 XML 是否属于当前图层及包含拓扑规则

位置：`MatchXmlByLayerName(...)` 选出 XML 后、创建输出目录之前（当前文件约第 5492 行）。

- 修改前：使用 `MatchXmlByLayerName(...)` 的返回路径，未在此处核实它是否是对应图层的 XML 以及是否包含目标拓扑检查项。
- 修改后：使用 `QFileInfo`、`QFile`、`QDomDocument` 校验：
  1. XML 文件名（不含扩展名）必须与 `standardLayer` 完全一致；
  2. 文件必须能够打开并解析为 XML；
  3. 必须存在 `check_item`，其 `id` 为 `112`，且 `name`（去除首尾空白）与传入的 `checkItemName` 一致。
- 任一条件不满足时，提前返回 `质检失败`、`err_exist = true`；在 `info.summary`、`info.description` 写明缺少图层专属 XML 或 112 拓扑规则，并在 `info.xml_path` 记录选中的路径。
- 目的：避免匹配回退到无关 XML（例如 `#比例尺.xml`），导致拓扑规则和容差配置缺失却继续执行。
- 兼容性：这项严格校验只加在该线要素拓扑入口，不改变其他算法的 XML 匹配函数。重命名 XML、使用通用文件名或将规则名改成不同文字，会使该入口拒绝执行。

## 关联文件与验证

- 多 SHP 路径处理和拓扑结果输出等修改在 `ui_class/ReadShp_interface.cpp`；道路拓扑判定在 `ui_class/ReadShp.cpp` 和新增的 `ui_class/road_topology_network.h`；参数映射在 `ui_class/QualityCheckConfig.cpp`。只移植本文件不包含这些修复。
- `tests/road_topology/test.cpp` 中的 `task cannot silently substitute another road XML` 和 `missing input is not a successful topology run` 分别覆盖上述两项行为。
- 本次整理未重新运行测试；此前道路拓扑回归记录为 41 项通过，另有永登县地图新版 2 的实际数据重放通过。
