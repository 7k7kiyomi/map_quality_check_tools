#pragma once

#ifndef ScaleCheckerNormativeDialog_H
#define ScaleCheckerNormativeDialog_H

#include <QtWidgets/QDialog>
#include "ui_ScaleCheckerNormativeDialog.h"
#include <QString>
#include<qgisinterface.h>
#include <qgsvectorlayer.h>
#include<qgsproject.h>
#include <qgshighlight.h>        // 用于要素高亮
#include <qgsfeatureiterator.h>  // 要素迭代器
#include <qgsgeometry.h>         // 几何处理
#include <qgscoordinatereferencesystem.h>  // QGIS坐标系类
#include <qgsrectangle.h>                  // QGIS矩形范围类

namespace Ui {
    class ScaleCheckerNormativeDialogClass;
}

class ScaleCheckerNormativeDialog : public QDialog
{
    Q_OBJECT

public:
    ScaleCheckerNormativeDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~ScaleCheckerNormativeDialog(void);

    // 设置QGIS接口（供插件主类调用）
    void setQgisInterface(QgisInterface* iface);

private slots:
    void on_loadGeoPdfButton_clicked();
    void on_loadXmlButton_clicked();
    void on_checkButton_clicked();

private:
    Ui::ScaleCheckerNormativeDialogClass* ui;
    QgisInterface* mIface = nullptr;           // QGIS接口指针

    // GeoPDF相关变量
    QString m_geoPdfPath;                      // GeoPDF文件路径
    QgsCoordinateReferenceSystem m_pdfCrs;     // GeoPDF的坐标系信息
    QgsRectangle m_pdfGeoExtent;               // GeoPDF的地理范围（边界框）
    QSize m_pdfPageSize;                       // GeoPDF的页面尺寸（像素）

    // XML相关变量
    QString m_xmlFilePath;                     // XML文件路径
    QString m_xmlTargetScale;                  // 从XML中提取的目标比例尺（格式：1:N）
    QString m_calculatedScale;                 // 计算得到的实际比例尺（格式：1:N）

    // 新增：用于高亮不规范要素
    //QgsHighlight* m_highlight = nullptr;
    // 记录不规范要素的 ID
    //QList<QgsFeatureId> m_invalidFeatureIds;

    /**
     * @brief 解析GeoPDF文件信息
     * @param pdfPath GeoPDF文件路径
     * @return 解析成功返回true，否则返回false
     * 提取GeoPDF的坐标系、地理范围和页面尺寸信息
     */
    bool readGeoPdfInfo(const QString& pdfPath);

    /**
     * @brief 计算GeoPDF的实际比例尺
     * @return 计算得到的比例尺字符串（格式：1:N），计算失败返回空字符串
     */
    QString calculateActualMapScale();

    /**
     * @brief 从XML文件中读取比例尺原始值
     * @param xmlPath XML文件路径
     * @param rawScale 输出参数，存储读取到的原始比例尺文本
     * @return 读取成功返回true，否则返回false
     */
    bool readScaleFromXml(const QString& xmlPath, QString& rawScale);

    /**
     * @brief 校验并标准化比例尺格式
     * @param rawText 原始比例尺文本
     * @param cleanedScale 输出参数，存储标准化后的比例尺（格式：1:N）
     * @return 格式有效返回true，否则返回false
     */
    bool isValidScaleFormat(const QString& rawText, QString& cleanedScale);

    /**
     * @brief 将对比结果导出为TXT文件
     * @param result 结果文本内容
     * @param actualScale 实际比例尺
     * @param targetScale 目标比例尺
     * @return 导出成功返回true，否则返回false
     */
    bool exportResultToTxt(const QString& result, const QString& actualScale, const QString& targetScale);
    bool saveShpAs(const QString& originalPath);
};
#endif //ScaleCheckerNormativeDialog_H
