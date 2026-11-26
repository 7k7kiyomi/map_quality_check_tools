#include "FileNameDialog.h"
#include "ui_FileNameDialog.h"
#include <QListWidgetItem>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QColor>

FileNameDialog::FileNameDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::FileNameDialog)
    , m_selectedFileName("")
{
    ui->setupUi(this);
    setWindowTitle("选择新建文件");
    ui->listWidget_filename->setAlternatingRowColors(true);
}

FileNameDialog::~FileNameDialog()
{
    delete ui;
}

QString FileNameDialog::getSelectedFileName() const
{
    return m_selectedFileName;
}

// 浏览CSV文件
void FileNameDialog::on_pushButton_filename_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(this, "选择CSV文件", "", "CSV Files (*.csv)");
    if (!filePath.isEmpty()) {
        ui->lineEdit_filepath->setText(filePath);
        loadCsvData(filePath);
    }
}

// 解析CSV并仅加载“标准图层”列，供新建质检方案时选择图层名称。
void FileNameDialog::loadCsvData(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "错误", "无法打开文件");
        return;
    }

    m_uniqueItems.clear();
    ui->listWidget_filename->clear();

    QTextStream in(&file);
    if (in.atEnd()) {
        QMessageBox::warning(this, "错误", "CSV文件为空");
        return;
    }

    // CSV 第一行必须是表头；按“标准图层”表头定位，避免将描述等其他列加入列表。
    QStringList headers = in.readLine().split(",");
    int standardLayerColumn = -1;
    for (int index = 0; index < headers.size(); ++index) {
        QString header = headers[index].trimmed();
        if (!header.isEmpty() && header.at(0) == QChar(0xFEFF)) {
            header.remove(0, 1); // 兼容 UTF-8 BOM
        }
        if (header == QStringLiteral("标准图层")) {
            standardLayerColumn = index;
            break;
        }
    }

    if (standardLayerColumn < 0) {
        QMessageBox::warning(this, "错误", "CSV中未找到“标准图层”列");
        return;
    }

    while (!in.atEnd()) {
        QString line = in.readLine();
        QStringList fields = line.split(",");
        if (fields.size() > standardLayerColumn) {
            const QString standardLayer = fields[standardLayerColumn].trimmed();
            if (!standardLayer.isEmpty() && !m_uniqueItems.contains(standardLayer)) {
                m_uniqueItems.insert(standardLayer);
                ui->listWidget_filename->addItem(standardLayer);
            }
        }
    }

    file.close();
}

// 查找并高亮
void FileNameDialog::on_pushButton_find_clicked()
{
    QString searchText = ui->lineEdit_filename->text().trimmed();
    clearHighlight();
    if (searchText.isEmpty()) return;

    int matchCount = 0;
    QColor highlightColor(255, 255, 153);

    for (int i = 0; i < ui->listWidget_filename->count(); ++i) {
        QListWidgetItem* item = ui->listWidget_filename->item(i);
        if (item->text().contains(searchText, Qt::CaseInsensitive)) {
            item->setBackground(QBrush(highlightColor));
            matchCount++;
            if (matchCount == 1) {
                ui->listWidget_filename->scrollToItem(item);
            }
        }
    }

    QMessageBox::information(this, "提示", QString("找到 %1 个匹配项").arg(matchCount));
}

// 清除高亮
void FileNameDialog::clearHighlight()
{
    for (int i = 0; i < ui->listWidget_filename->count(); ++i) {
        QListWidgetItem* item = ui->listWidget_filename->item(i);
        item->setBackground(QBrush());
    }
}

// 确认选择
void FileNameDialog::on_pushButton_ok_clicked()
{
    QListWidgetItem* currentItem = ui->listWidget_filename->currentItem();
    if (currentItem) {
        m_selectedFileName = currentItem->text();
        accept();
    }
    else {
        QMessageBox::warning(this, "警告", "请先选择一个项");
    }
}

// 关闭窗口
void FileNameDialog::on_pushButton_close_clicked()
{
    reject();
}
