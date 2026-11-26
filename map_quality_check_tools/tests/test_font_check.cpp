#include <iostream>
#include <string>
#include <QFile>
#include <QDir>
#include <QApplication>
#include <qgsapplication.h>
#include "../algorithm/se_annotation_check_interface.h"
#include "../algorithm/se_annotation_advanced_check_interface.h"

// --- 测试字体 201 ---
void test_font_check()
{
    std::cout << "\n\n--- Running Font Check Test ---" << std::endl;

    const char *shp_path = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/泉_标注.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/泉_标注.xml";
    const char *report_path = "D:/Media/Downloads/测试输出/font_check_report.json";
    const char *result_path = "D:/Media/Downloads/测试输出/font_check_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/font_check_log日志.log";

    std::cout << "Starting AnnotationFontCheck..." << std::endl;
    std::cout << "Input SHP: " << shp_path << std::endl;

    SE_Error result = AnnotationFontCheck(
        shp_path,
        schema_path,
        145000,
        result_path,
        log_path,
        report_path);

    std::cout << "Font check finished with result code: " << result << std::endl
              << std::endl;

    if (result == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << report_path << std::endl;
    }
}

// --- 测试颜色 203 ---
void test_color_check()
{
    std::cout << "\n\n--- Running Color Check Test ---" << std::endl;

    const char *shp_path = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/泉_标注.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/泉_标注.xml";
    const char *report_path = "D:/Media/Downloads/测试输出/font_color_check_report.json";
    const char *result_path = "D:/Media/Downloads/测试输出/font_color_check_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/font_color_check_log日志.log";

    std::cout << "Starting AnnotationColorCheck..." << std::endl;
    std::cout << "Input SHP: " << shp_path << std::endl;

    SE_Error result = AnnotationColorCheck(
        shp_path,
        schema_path,
        145000,
        result_path,
        log_path,
        report_path);

    std::cout << "Color check finished with result code: " << result << std::endl
              << std::endl;

    if (result == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << report_path << std::endl;
    }
}

// --- 测试字号 202 ---
void test_font_size_check()
{
    std::cout << "\n\n--- Running Font Size Check Test ---" << std::endl;

    const char *shp_path = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/泉_标注.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/泉_标注.xml";
    const char *report_path = "D:/Media/Downloads/测试输出/font_size_check_report.json";
    const char *result_path = "D:/Media/Downloads/测试输出/font_size_check_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/font_size_check_log日志.log";

    std::cout << "Starting AnnotationFontSizeCheck..." << std::endl;
    std::cout << "Input SHP: " << shp_path << std::endl;

    SE_Error result = AnnotationFontSizeCheck(
        shp_path,
        schema_path,
        145000,
        result_path,
        log_path,
        report_path);

    std::cout << "Font size check finished with result code: " << result << std::endl
              << std::endl;

    if (result == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << report_path << std::endl;
    }
}

// 测试斜体 205
void test_italic_check()
{
    std::cout << "\n\n--- Running Italic Check Test ---" << std::endl;

    const char *shp_path = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/泉_标注.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/泉_标注.xml";
    const char *report_path = "D:/Media/Downloads/测试输出/italic_check_report.json";
    const char *result_path = "D:/Media/Downloads/测试输出/italic_check_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/italic_check_log日志.log";

    std::cout << "Starting AnnotationItalicCheck..." << std::endl;
    std::cout << "Input SHP: " << shp_path << std::endl;

    SE_Error result = AnnotationItalicCheck(
        shp_path,
        schema_path,
        145000,
        result_path,
        log_path,
        report_path);

    std::cout << "Italic check finished with result code: " << result << std::endl
              << std::endl;

    if (result == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << report_path << std::endl;
    }
}

// --- 测试重叠检查 206 ---
void test_overlap_check()
{
    std::cout << "\n\n--- Running Overlap Check Test ---" << std::endl;

    const char *symbol_paths =
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/行政村.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/自然村.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/乡镇级驻地.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/县级驻地.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/景点景区.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/泉.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/高速出入口.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/公路隧道口.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/火车站.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/井.shp";
    const char *anno_paths =
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/县级驻地_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/乡镇级驻地_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/行政村_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/自然村_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/景点景区_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/泉_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/高速出入口_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/高速公路_代码标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/高速公路_名称标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/高速铁路_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/国道 代码标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/火车站_标注.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/井_标注.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/全局重叠检查规则.xml";
    const char *report_path = "D:/Media/Downloads/测试输出/overlap_check_report.json";
    const char *result_path = "D:/Media/Downloads/测试输出/overlap_check_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/overlap_check_log日志.log";

    std::cout << "Starting AnnotationOverlapCheck..." << std::endl;

    SE_Error result = AnnotationOverlapCheck(
        symbol_paths,
        anno_paths,
        schema_path,
        145000,
        result_path,
        log_path,
        report_path);

    std::cout << "Overlap check finished with result code: " << result << std::endl
              << std::endl;

    if (result == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << report_path << std::endl;
    }
}

// --- 200 ---
void test_feature_matching()
{
    std::cout << "\n\n--- Running Feature Matching Test ---" << std::endl;

    const char *standardLayer = "地理名称";
    const char *gdbPath = "D:/Media/Downloads/test_parse/永登县地图——新版2_gdb_shp/AANP.shp";
    const char *actualPath = "";
    const char *annoPath = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/地理名称.shp";
    const char *xmlPath = "D:/develop/projects/map_checker/xml/地理名称.xml";
    const char *outputPath = "D:/Media/Downloads/测试输出/temp_standard_layer_地理名称.shp";
    // const char *standardLayer = "高速公路";
    // const char *gdbPath = "D:/Media/Downloads/test_parse/永登县地图——新版2_gdb_shp/LRDL.shp";
    // const char *actualPath = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/高速公路.shp";
    // const char *annoPath = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/高速公路_代码标注.shp";
    // const char *xmlPath = "D:/develop/projects/map_checker/xml/高速公路_代码标注.xml";
    // const char *outputPath = "D:/Media/Downloads/测试输出/temp_standard_layer_高速公路.shp";

    if (!QFile::exists(QString::fromUtf8(gdbPath)))
    {
        std::cout << "ERROR: GDB file not found: " << gdbPath << std::endl;
        return;
    }
    if (!QFile::exists(QString::fromUtf8(annoPath)))
    {
        std::cout << "ERROR: Anno layer not found: " << annoPath << std::endl;
        return;
    }

    std::cout << "All input files validated, starting matching..." << std::endl;

    SE_Error rc = AnnotationMatchFeatures(
        standardLayer,
        gdbPath,
        xmlPath,
        actualPath,
        annoPath,
        145000,
        outputPath);

    std::cout << "Feature matching returned: " << rc << std::endl;

    if (rc == SE_ERROR_NONE)
    {
        std::cout << "SUCCESS: Check output file" << std::endl;
    }
    else
    {
        std::cout << "FAILED: Error code " << rc << std::endl;
    }
}

// --- 测试符号-注记匹配（ID=210） ---
void test_symbol_annotation_match_check()
{
    std::cout << "\n\n--- Running Symbol-Annotation Match Check Test (ID=210) ---" << std::endl;

    const char *match_result_path = "D:/Media/Downloads/test_output/注记匹配/temp_standard_layer_地理名称.shp";
    const char *actual_layer_path = "";
    const char *schema_path = "D:/develop/projects/map_checker/xml/地理名称.xml";
    const char *report_path = "D:/Media/Downloads/测试输出/symbol_annotation_match_report.json";
    const char *result_path = "D:/Media/Downloads/测试输出/symbol_annotation_match_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/symbol_annotation_match_log日志.log";
    // const char *match_result_path = "D:/Media/Downloads/test_output/注记匹配/temp_standard_layer_行政村.shp";
    // const char *actual_layer_path = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/行政村.shp";
    // const char *schema_path = "D:/develop/projects/map_checker/xml/行政村_标注.xml";
    // const char *report_path = "D:/Media/Downloads/测试输出/symbol_annotation_match_report.json";
    // const char *result_path = "D:/Media/Downloads/测试输出/symbol_annotation_match_result结果.shp";
    // const char *log_path = "D:/Media/Downloads/测试输出/symbol_annotation_match_log日志.log";

    std::cout << "Starting SymbolAnnotationMatchCheck..." << std::endl;
    std::cout << "Match result layer: " << match_result_path << std::endl;

    SE_Error rc = SymbolAnnotationMatchCheck(
        match_result_path,
        actual_layer_path,
        schema_path,
        145000,
        result_path,
        log_path,
        report_path);

    std::cout << "Symbol-Annotation match check finished with result code: " << rc << std::endl
              << std::endl;

    if (rc == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << report_path << std::endl;
    }
}

// --- 测试字距检查（ID=204） ---
void test_char_spacing_check()
{
    std::cout << "\n\n--- Running Char Spacing Check Test (ID=204) ---" << std::endl;

    const char *match_result_path = "D:/Media/Downloads/test_output/注记匹配/temp_standard_layer_等外.shp";
    const char *gdb_layer_path = "D:/Media/Downloads/test_parse/永登县地图——新版2_gdb_shp/HYDL_HL.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/等外河流_标注.xml";
    const char *report_path = "D:/Media/Downloads/测试输出/char_spacing_report.json";
    const char *result_path = "D:/Media/Downloads/测试输出/char_spacing_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/char_spacing_log日志.log";

    std::cout << "Starting CharSpacingCheck..." << std::endl;
    std::cout << "Match result layer: " << match_result_path << std::endl;
    std::cout << "GDB layer: " << gdb_layer_path << std::endl;

    SE_Error rc = CharSpacingCheck(
        match_result_path,
        gdb_layer_path,
        schema_path,
        145000,
        result_path,
        log_path,
        report_path);

    std::cout << "Char spacing check finished with result code: " << rc << std::endl
              << std::endl;

    if (rc == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << report_path << std::endl;
    }
}

// --- 测试注记组距检查（ID=207） ---
void test_group_spacing_check()
{
    std::cout << "\n\n--- Running Group Spacing Check Test (ID=207) ---" << std::endl;

    const char *match_result_path = "D:/Media/Downloads/test_output/注记匹配/temp_standard_layer_干渠.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/干渠_标注.xml";
    const char *result_path = "D:/Media/Downloads/测试输出/group_spacing_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/group_spacing_log日志.log";
    const char *json_out_path = "D:/Media/Downloads/测试输出/group_spacing_report.json";

    std::cout << "Starting GroupSpacingCheck..." << std::endl;
    std::cout << "Match result layer: " << match_result_path << std::endl;

    SE_Error rc = GroupSpacingCheck(
        match_result_path,
        schema_path,
        145000,
        result_path,
        log_path,
        json_out_path);

    std::cout << "Group spacing check finished with result code: " << rc << std::endl;

    if (rc == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << json_out_path << std::endl;
    }
    else
    {
        std::cout << "FAILED: Error code " << rc << std::endl;
    }
}

// 新增：测试面要素等间距标注检查（ID=208）
void test_equal_spacing_check()
{
    std::cout << "\n\n--- Running Equal Spacing Check Test (ID=208) ---" << std::endl;

    const char *match_result_path = "D:/Media/Downloads/test_output/注记匹配/temp_standard_layer_行政区划名普色（乡镇级表面注记）.shp";
    const char *gdb_layer_path = "D:/Media/Downloads/test_parse/永登县地图——新版2_gdb_shp/BOUA6.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/乡镇级表面标注_标注.xml";
    const char *result_path = "D:/Media/Downloads/测试输出/equal_spacing_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/equal_spacing_log日志.log";
    const char *json_path = "D:/Media/Downloads/测试输出/equal_spacing_report.json";

    std::cout << "Starting EqualSpacingCheck..." << std::endl;
    std::cout << "Match result layer: " << match_result_path << std::endl;
    std::cout << "GDB layer: " << gdb_layer_path << std::endl;

    SE_Error rc = EqualSpacingCheck(
        match_result_path,
        gdb_layer_path,
        schema_path,
        145000,
        result_path,
        log_path,
        json_path);

    std::cout << "Equal spacing check finished with result code: " << rc << std::endl;

    if (rc == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << json_path << std::endl;
    }
    else
    {
        std::cout << "FAILED: Error code " << rc << std::endl;
    }
}

// --- 测试符号方向性检查（ID=209） ---
void test_symbol_direction_check_209()
{
    std::cout << "\n\n--- Running Symbol Direction Check Test (ID=209) ---" << std::endl;

    const char *symbol_layer_path =
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/渠流向_LineString.shp;"
        "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/渠流向_Polygon.shp";
    const char *associated_layers = "D:/Media/Downloads/test_parse/永登县地图——新版2_shp/沟渠.shp";
    const char *schema_path = "D:/develop/projects/map_checker/xml/渠流向.xml";
    const char *result_path = "D:/Media/Downloads/测试输出/symbol_direction_result结果.shp";
    const char *log_path = "D:/Media/Downloads/测试输出/symbol_direction_log日志.log";
    const char *json_path = "D:/Media/Downloads/测试输出/symbol_direction_report.json";

    std::cout << "Starting SymbolDirectionCheck..." << std::endl;
    std::cout << "Symbol layer: " << symbol_layer_path << std::endl;

    SE_Error rc = SymbolDirectionCheck(
        symbol_layer_path,
        associated_layers,
        schema_path,
        145000,
        result_path,
        log_path,
        json_path);

    std::cout << "SymbolDirectionCheck finished with result code: " << rc << std::endl;

    if (rc == SE_ERROR_NONE)
    {
        std::cout << "JSON report written to: " << json_path << std::endl;
    }
    else
    {
        std::cout << "FAILED: Error code " << rc << std::endl;
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QgsApplication::setPrefixPath(QStringLiteral("D:/develop/software/QGIS_plugin_env/MPQIS"), true);
    QgsApplication::initQgis();

    std::cout << "QGIS Application Initialized." << std::endl;

    // test_font_check();
    // test_color_check();
    // test_font_size_check();
    // test_italic_check();
    // test_overlap_check();
    // test_feature_matching();
    test_symbol_annotation_match_check();
    // test_char_spacing_check();
    // test_group_spacing_check();
    // test_equal_spacing_check();
    // test_symbol_direction_check_209();

    QgsApplication::exitQgis();

    std::cout << "Press Enter to exit...";
    std::cin.get();

    return 0;
}