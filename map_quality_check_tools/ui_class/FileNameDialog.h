#pragma once
#ifndef FILE_NAME_DIALOG_H
#define FILE_NAME_DIALOG_H

#include <QDialog>
#include <QString>
#include <QSet>

namespace Ui {
    class FileNameDialog;
}

class FileNameDialog : public QDialog
{
    Q_OBJECT

public:
    FileNameDialog(QWidget* parent = nullptr);
    ~FileNameDialog() override;

    QString getSelectedFileName() const;

private slots:
    void on_pushButton_find_clicked();
    void on_pushButton_ok_clicked();
    void on_pushButton_close_clicked();
    void on_pushButton_filename_clicked();

private:
    void loadCsvData(const QString& filePath);
    void clearHighlight();

    Ui::FileNameDialog* ui;
    QString m_selectedFileName;
    QSet<QString> m_uniqueItems; // 用于去重
};
#endif // FILE_NAME_DIALOG_H