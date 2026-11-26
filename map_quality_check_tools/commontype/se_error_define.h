#ifndef SE_ERROR_DEFINE_H
#define SE_ERROR_DEFINE_H

#include <QString>
#include <QStringList>
#include <QPointF>

/*定义接口返回值错误编码*/
typedef int SE_Error;

#define SE_ERROR_NONE 0                      /*成功*/
#define SE_ERROR_NOT_ENOUGH_DATA 1           /*没有足够的反序列化数据*/
#define SE_ERROR_NOT_ENOUGH_MEMORY 2         /*内存不足*/
#define SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE 3 /*几何类型不支持*/
#define SE_ERROR_UNSUPPORTED_OPERATION 4     /*操作不支持*/
#define SE_ERROR_CORRUPT_DATA 5              /*错误数据*/
#define SE_ERROR_FAILURE 6                   /*失败*/
#define SE_ERROR_UNSUPPORTED_SRS 7           /*空间参考系不支持*/
#define SE_ERROR_INVALID_HANDLE 8            /*不支持的句柄*/
#define SE_ERROR_FILE_NOT_FOUND 9            /*文件不存在*/
#define SE_ERROR_INVALID_PARAMETER 10        /*参数无效*/
#define SE_ERROR_WRITE_FILE_FAILED 11        /*写文件失败*/
#define SE_ERROR_BUFFER_TOO_SMALL 12         // 缓冲区太小

//* 错误类型枚举，补充 SizeError（字距/间距相关）以匹配代码中使用 */
enum class AnnotationErrorType
{
    FontError = 0,     // 字体或内容类错误
    FontSizeError = 1, // 字号错误
    ColorError = 2,    // 颜色错误
    ItalicError = 3,   // 斜体错误
    OverlapError = 4,  // 重叠/距离错误
    SizeError = 5      // 字距/间距（204）错误
};

// 检查结果结构
struct AnnotationCheckResult
{
    int geometryType = 0;          // QgsWkbTypes::GeometryType
    QString featureId;             // 要素ID
    AnnotationErrorType errorType; // 错误类型
    QString errorMessage;          // 错误描述
    QString actualValue;           // 实际值
    QString expectedValue;         // 期望值
    QPointF position;              // 错误位置
    QString layerName;             // 图层名称
};

// 质检规则项
struct CheckRuleItem
{
    QString name;  // 规则名称
    QString value; // 期望值
    QString unit;  // 单位（可选）
};

// 字体映射结构（简化版）
struct FontMapping
{
    QString chineseName; // 中文字体名
    QString englishName; // 英文字体名
    QString fileName;    // 文件名（可选）
    QString psName;      // PS名称（可选）
};

#endif // SE_ERROR_DEFINE_H
