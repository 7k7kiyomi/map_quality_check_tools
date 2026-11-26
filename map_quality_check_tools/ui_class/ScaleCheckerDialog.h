#pragma once

#ifndef SCALECHECKERDIALOG_H
#define SCALECHECKERDIALOG_H

#include <QtWidgets/QDialog>
#include "ui_ScaleCheckerDialog.h"
#include <QFileDialog>
#include <QGroupBox>
#include <QMessageBox>
#include <QList>             // 保留QList
#include <qgspointxy.h>      // 保留点坐标类
#include <QTextEdit>

namespace Ui {
    class ScaleCheckerDialog;
}

class ScaleCheckerDialog : public QDialog
{
    Q_OBJECT

public:
    ScaleCheckerDialog(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~ScaleCheckerDialog(void);

private slots:
    void on_loadShpButton_clicked();
    void on_checkButton_clicked();
    void on_loadGeoPdfButton_clicked();//加载内图廓shp
    void updateLog(const QString& log);//更新运行日志
    void on_loadjsonButton_clicked();

private:
    Ui::ScaleCheckerDialogClass* ui;

    int errorFeatureId = -1;

    // 文件路径
    QString m_mapFrameShpPath;             // 内图廓SHP路径
    QString m_shpFilePath;
    QString m_jsonOutputPath;

    // 比例尺信息
    QString m_shpScaleValue;
    QString m_calculatedScale;             // 计算的比例尺

    // 内图廓相关数据
    double m_mapFrameWidth;//内图廓宽度
    double m_mapFrameAttrPixelWidth = 0.0;  // 属性表中读取的像素宽度
    double m_mapFrameAttrPixelHeight = 0.0; // 属性表中读取的像素高度
    QList<QgsPointXY> m_mapFramePoints;    // 内图廓四个顶点

    // 运行日志的QTextEdit指针
    QTextEdit* m_logTextEdit;

    // 修改内图廓提取函数参数（从文件路径读取，而非QGIS图层）
    bool extractMapFramePointsFromShp(const QString& shpPath, QList<QgsPointXY>& points);

    // 保留核心函数
    void cleanMapFrameData();
    double calculateMapFrameWidth(const QList<QgsPointXY>& points);

    // 删除高亮相关函数
    // void markInconsistentFeatures(const QgsVectorLayer* sourceLayer, const QList<QgsFeatureId>& inconsistentFeatureIds);

    // 保留其他核心函数
    bool isValidScaleText(const QString& text, QString* scaleValue = nullptr); //验证比例尺文本格式
    QString calculateActualMapScale();

    // 保留结果导出和文件操作函数
    bool exportResultToJson(const QString& result, const QString& shpValue, const QString& xmlValue, const QString& pdfValue);
    bool saveShpAs(const QString& originalPath, const QString& targetDir);
    void extractTextConteAndMatchXml();
   // QString matchXmlValueByText(const QString& text);

};
#endif // SCALECHECKERDIALOG_H