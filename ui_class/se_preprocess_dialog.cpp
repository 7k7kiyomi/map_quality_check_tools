#include "se_preprocess_dialog.h"
#include "postgis_shared_connection.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QTextStream>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>

// QGIS includes
#include "qgsapplication.h"
#include "qgscoordinatetransform.h"
#include "qgsdatasourceuri.h"
#include "qgsfeature.h"
#include "qgsfeaturerequest.h"
#include "qgsgeometry.h"
#include "qgsproject.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorfilewriter.h"
#include "qgsvectorlayer.h"
#include "qgswkbtypes.h"

namespace
{
class PostgisQueryConnection
{
public:
    PostgisQueryConnection(const PostgisSourceSettings& settings, quintptr ownerId)
    {
        const QString sharedName = mapProductManagerSharedPostgisConnectionName();
        if (QSqlDatabase::contains(sharedName))
        {
            QSqlDatabase sharedDatabase = QSqlDatabase::database(sharedName, false);
            if (sharedDatabase.isValid() && sharedDatabase.isOpen()
                && sharedDatabase.hostName() == settings.host
                && sharedDatabase.port() == settings.port
                && sharedDatabase.databaseName() == settings.database
                && sharedDatabase.userName() == settings.user)
            {
                m_database = sharedDatabase;
                m_usesSharedConnection = true;
                return;
            }
        }

        m_connectionName = QStringLiteral("PreprocessPostgis_%1")
            .arg(ownerId);
        if (QSqlDatabase::contains(m_connectionName))
            QSqlDatabase::removeDatabase(m_connectionName);
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), m_connectionName);
        m_database.setHostName(settings.host);
        m_database.setPort(settings.port);
        m_database.setDatabaseName(settings.database);
        m_database.setUserName(settings.user);
        m_database.setPassword(settings.password);
        if (!m_database.open())
            m_error = m_database.lastError().text();
    }

    ~PostgisQueryConnection()
    {
        if (m_usesSharedConnection)
            return;
        if (m_database.isValid())
            m_database.close();
        m_database = QSqlDatabase();
        if (!m_connectionName.isEmpty())
            QSqlDatabase::removeDatabase(m_connectionName);
    }

    bool isOpen() const { return m_database.isOpen(); }
    bool usesSharedConnection() const { return m_usesSharedConnection; }
    QString error() const { return m_error; }
    QSqlDatabase database() const { return m_database; }

private:
    QSqlDatabase m_database;
    QString m_connectionName;
    QString m_error;
    bool m_usesSharedConnection = false;
};

QString safeShapefileBaseName(const QString& name)
{
    QString result = name.trimmed();
    result.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    return result.isEmpty() ? QStringLiteral("layer") : result;
}

QString outputLayerNameFromPostgisTable(const QString& tableName, const QString& pdfBaseName)
{
    // GDB 入库表名：<地图名>_v<版本>_<入库图层名>，例如：
    // 永登县_v1_a_lvyoujingqu -> a_lvyoujingqu。
    const QRegularExpression importedGdbPattern(
        QStringLiteral("^%1_v\\d+_(.+)$").arg(QRegularExpression::escape(pdfBaseName)));
    const QRegularExpressionMatch match = importedGdbPattern.match(tableName);
    if (!match.hasMatch())
        return safeShapefileBaseName(tableName);

    // 按入库图层名原样导出；PostgreSQL 已将未加引号的名称规范为小写。
    const QString originalLayerName = match.captured(1);
    return safeShapefileBaseName(originalLayerName);
}

QgsGeometry lineFrameToPolygon(const QgsPolylineXY& line)
{
    if (line.size() < 3)
        return QgsGeometry();

    QgsPolylineXY ring = line;
    if (ring.first() != ring.last())
        ring.append(ring.first());

    QgsPolygonXY polygon;
    polygon.append(ring);
    const QgsGeometry result = QgsGeometry::fromPolygonXY(polygon);
    return result.isGeosValid() ? result : QgsGeometry();
}

QgsGeometry frameGeometryToArea(const QgsGeometry& geometry)
{
    if (geometry.isNull() || geometry.isEmpty())
        return QgsGeometry();
    if (QgsWkbTypes::geometryType(geometry.wkbType()) != QgsWkbTypes::LineGeometry)
        return geometry;

    if (!geometry.isMultipart())
        return lineFrameToPolygon(geometry.asPolyline());

    QgsGeometry result;
    const QgsMultiPolylineXY lines = geometry.asMultiPolyline();
    for (const QgsPolylineXY& line : lines)
    {
        const QgsGeometry polygon = lineFrameToPolygon(line);
        if (polygon.isNull())
            continue;
        result = result.isNull() ? polygon : result.combine(polygon);
    }
    return result;
}

}

CSE_PreprocessDialog::CSE_PreprocessDialog(QWidget *parent, Qt::WindowFlags fl)
    : QDialog(parent, fl), m_pCurrentProcess(nullptr), m_currentStep(0), m_totalSteps(0),
      m_isSymbolProcessing(false), m_failedSteps(0)
{
    ui.setupUi(this);

    ui.groupBox_Symbol->setTitle(QString::fromUtf8(u8"符号库PDF处理"));
    ui.label_SymbolStep1->setText(QString::fromUtf8(u8"<b>步骤一：选择符号库PDF文件</b>"));
    ui.label_SymbolPDFPaths->setText(QString::fromUtf8(u8"PDF路径："));
    ui.pushButton_AddSymbolPDF->setText(QString::fromUtf8(u8"添加PDF"));
    ui.label_SymbolDescription->setText(QString::fromUtf8(
        u8"说明：本工具将解析符号库PDF，自动建立符号与要素名称的对应关系，"
        u8"并输出 standard_legend_table.csv、standard_legend_symbols.geojson 及可在QGIS中查看的分组结果。"));

    // 创建并连接进程
    m_pCurrentProcess = new QProcess(this);
    connect(m_pCurrentProcess, &QProcess::readyReadStandardOutput, this, &CSE_PreprocessDialog::onProcessReadyReadStandardOutput);
    connect(m_pCurrentProcess, &QProcess::readyReadStandardError, this, &CSE_PreprocessDialog::onProcessReadyReadStandardError);
    connect(m_pCurrentProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &CSE_PreprocessDialog::onProcessFinished);

    // 初始化UI状态
    updateUIState(false);

    // 连接信号和槽
    connectSignals();

    // 恢复上次的设置
    restoreState();
}

CSE_PreprocessDialog::~CSE_PreprocessDialog()
{
    if (m_pCurrentProcess && m_pCurrentProcess->state() != QProcess::NotRunning)
    {
        m_pCurrentProcess->kill();
        m_pCurrentProcess->waitForFinished(3000);
    }

    if (m_pCurrentProcess)
    {
        delete m_pCurrentProcess;
        m_pCurrentProcess = nullptr;
    }

    saveState();
}

void CSE_PreprocessDialog::connectSignals()
{
    // PDF相关信号连接
    connect(ui.pushButton_AddPDF, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onAddPDFClicked);
    connect(ui.pushButton_RemovePDF, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onRemovePDFClicked);
    connect(ui.pushButton_ClearPDF, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onClearPDFClicked);
    connect(ui.pushButton_BrowsePDFOutput, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onBrowsePDFOutputClicked);

    // GDB相关信号连接
    connect(ui.pushButton_BrowseSourceGDB, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onAddGDBClicked);
    connect(ui.radioButton_SourceGDB, &QRadioButton::toggled,
            this, &CSE_PreprocessDialog::onSourceDataTypeChanged);
    connect(ui.radioButton_SourcePostgis, &QRadioButton::toggled,
            this, &CSE_PreprocessDialog::onSourceDataTypeChanged);
    connect(ui.pushButton_ConnectPostgis, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onConnectPostgisClicked);

    // 控制按钮信号连接
    connect(ui.pushButton_StartProcess, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onStartProcessClicked);
    connect(ui.pushButton_Cancel, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onCancelClicked);
    connect(ui.pushButton_Close, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onCloseClicked);

    // 符号库相关信号连接
    connect(ui.pushButton_AddSymbolPDF, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onAddSymbolPDFClicked);
    connect(ui.pushButton_RemoveSymbolPDF, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onRemoveSymbolPDFClicked);
    connect(ui.pushButton_ClearSymbolPDF, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onClearSymbolPDFClicked);
    connect(ui.pushButton_BrowseSymbolOutput, &QPushButton::clicked,
            this, &CSE_PreprocessDialog::onBrowseSymbolOutputClicked);
}

PreprocessParams CSE_PreprocessDialog::getPreprocessParams() const
{
    PreprocessParams params;

    // GeoPDF设置
    params.pdfOutputPath = ui.lineEdit_PDFOutputPath->text();

    // 获取PDF路径列表
    for (int i = 0; i < ui.listWidget_PDFPaths->count(); ++i)
    {
        params.pdfPaths.append(ui.listWidget_PDFPaths->item(i)->text());
    }

    // GDB设置
    params.sourceGdbPath = ui.lineEdit_SourceGDB->text();
    params.sourceDataType = currentSourceDataType();
    params.postgisSource = m_postgisSource;

    // 符号库设置
    for (int i = 0; i < ui.listWidget_SymbolPDFPaths->count(); ++i)
    {
        params.symbolPdfPaths.append(ui.listWidget_SymbolPDFPaths->item(i)->text());
    }
    params.symbolOutputPath = ui.lineEdit_SymbolOutputPath->text();

    return params;
}

void CSE_PreprocessDialog::setPreprocessParams(const PreprocessParams &params)
{
    // 设置GeoPDF参数
    ui.lineEdit_PDFOutputPath->setText(params.pdfOutputPath);

    // 设置PDF路径列表
    ui.listWidget_PDFPaths->clear();
    ui.listWidget_PDFPaths->addItems(params.pdfPaths);

    // 设置GDB参数
    ui.lineEdit_SourceGDB->setText(params.sourceGdbPath);
    m_postgisSource = params.postgisSource;
    if (params.sourceDataType == SourceDataType::Postgis)
        ui.radioButton_SourcePostgis->setChecked(true);
    else
        ui.radioButton_SourceGDB->setChecked(true);
    updateSourceDataUi();

    // 设置符号库参数
    ui.listWidget_SymbolPDFPaths->clear();
    ui.listWidget_SymbolPDFPaths->addItems(params.symbolPdfPaths);
    ui.lineEdit_SymbolOutputPath->setText(params.symbolOutputPath);

    m_params = params;
}

void CSE_PreprocessDialog::onAddPDFClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("选择PDF文件"),
        QString(),
        tr("PDF文件 (*.pdf)"));

    if (!files.isEmpty())
    {
        ui.listWidget_PDFPaths->addItems(files);
    }
}

void CSE_PreprocessDialog::onRemovePDFClicked()
{
    int currentRow = ui.listWidget_PDFPaths->currentRow();
    if (currentRow >= 0)
    {
        delete ui.listWidget_PDFPaths->takeItem(currentRow);
    }
}

void CSE_PreprocessDialog::onClearPDFClicked()
{
    ui.listWidget_PDFPaths->clear();
}

void CSE_PreprocessDialog::onBrowsePDFOutputClicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("选择PDF输出目录"),
        ui.lineEdit_PDFOutputPath->text());

    if (!dir.isEmpty())
    {
        ui.lineEdit_PDFOutputPath->setText(dir);
    }
}

void CSE_PreprocessDialog::onAddGDBClicked()
{
    QString file = QFileDialog::getExistingDirectory(
        this,
        tr("选择完整的全区GDB数据源"),
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!file.isEmpty())
    {
        // 确保以 .gdb 结尾（忽略大小写）
        if (!file.toLower().endsWith(".gdb"))
        {
            QMessageBox::warning(this, tr("路径错误"), tr("请选择以 .gdb 结尾的目录（FileGDB）"));
            return;
        }
        ui.lineEdit_SourceGDB->setText(file);
    }
}

SourceDataType CSE_PreprocessDialog::currentSourceDataType() const
{
    if (ui.radioButton_SourcePostgis->isChecked())
        return SourceDataType::Postgis;
    return ui.radioButton_SourceGDB->isChecked() ? SourceDataType::LocalGdb : SourceDataType::None;
}

bool CSE_PreprocessDialog::hasSourceData() const
{
    switch (currentSourceDataType())
    {
    case SourceDataType::LocalGdb:
        return !ui.lineEdit_SourceGDB->text().trimmed().isEmpty();
    case SourceDataType::Postgis:
        return !m_postgisSource.host.trimmed().isEmpty()
            && !m_postgisSource.database.trimmed().isEmpty()
            && !m_postgisSource.user.trimmed().isEmpty()
            && m_postgisSource.themeId > 0;
    case SourceDataType::None:
        return false;
    }
    return false;
}

int CSE_PreprocessDialog::mapStepsPerPdf() const
{
    return 2 + (hasSourceData() ? 1 : 0);
}

QString CSE_PreprocessDialog::postgisLayersDisplayText() const
{
    const int layerCount = !m_availablePostgisLayers.isEmpty()
        ? m_availablePostgisLayers.size()
        : m_postgisSource.layers.size();
    if (m_postgisSource.themeId <= 0)
        return QString();

    if (m_postgisSource.themeScope == PostgisThemeScope::ThemeContent)
    {
        return layerCount > 0
            ? tr("主题“%1” / 内容“%2”：%3 个空间图层")
                  .arg(m_postgisSource.themeName, m_postgisSource.themeContentName)
                  .arg(layerCount)
            : tr("主题“%1” / 内容“%2”").arg(m_postgisSource.themeName,
                                                m_postgisSource.themeContentName);
    }
    return layerCount > 0
        ? tr("主题“%1”的全部内容：%2 个空间图层")
              .arg(m_postgisSource.themeName).arg(layerCount)
        : tr("主题“%1”的全部内容").arg(m_postgisSource.themeName);
}

void CSE_PreprocessDialog::updateSourceDataUi()
{
    const bool isPostgis = currentSourceDataType() == SourceDataType::Postgis;
    ui.page_SourceGDB->setVisible(!isPostgis);
    ui.page_SourcePostgis->setVisible(isPostgis);
    ui.widget_SourceDataPages->updateGeometry();

    ui.lineEdit_PostgisLayers->setText(postgisLayersDisplayText());

    if (m_postgisSource.host.trimmed().isEmpty())
    {
        ui.label_PostgisConnectionStatus->setText(tr("未连接数据库"));
        ui.label_PostgisConnectionStatus->setStyleSheet(QStringLiteral("color: #666666;"));
        ui.pushButton_ConnectPostgis->setText(tr("连接 PostGIS"));
    }
    else
    {
        const QSqlDatabase sharedDatabase = QSqlDatabase::contains(
            mapProductManagerSharedPostgisConnectionName())
            ? QSqlDatabase::database(mapProductManagerSharedPostgisConnectionName(), false)
            : QSqlDatabase();
        const bool reusingDownloadConnection = sharedDatabase.isValid() && sharedDatabase.isOpen()
            && sharedDatabase.hostName() == m_postgisSource.host
            && sharedDatabase.port() == m_postgisSource.port
            && sharedDatabase.databaseName() == m_postgisSource.database
            && sharedDatabase.userName() == m_postgisSource.user;
        ui.label_PostgisConnectionStatus->setText(reusingDownloadConnection
            ? tr("已复用数据库连接：%1:%2 / %3 / %4")
                  .arg(m_postgisSource.host)
                  .arg(m_postgisSource.port)
                  .arg(m_postgisSource.database, m_postgisSource.schema)
            : tr("已配置：%1:%2 / %3 / %4")
                  .arg(m_postgisSource.host)
                  .arg(m_postgisSource.port)
                  .arg(m_postgisSource.database, m_postgisSource.schema));
        ui.label_PostgisConnectionStatus->setStyleSheet(QStringLiteral("color: #19703a;"));
        ui.pushButton_ConnectPostgis->setText(reusingDownloadConnection
            ? tr("选择主题") : tr("连接 PostGIS"));
    }
}

void CSE_PreprocessDialog::onSourceDataTypeChanged()
{
    updateSourceDataUi();
}

void CSE_PreprocessDialog::onConnectPostgisClicked()
{
    PostgisSourceSettings dialogSettings = m_postgisSource;
    if (QSqlDatabase::contains(mapProductManagerSharedPostgisConnectionName()))
    {
        const QSqlDatabase sharedDatabase = QSqlDatabase::database(
            mapProductManagerSharedPostgisConnectionName(), false);
        if (sharedDatabase.isValid() && sharedDatabase.isOpen())
        {
            // 以下载页当前会话为准，避免预处理窗口因较早创建而持有过期配置。
            dialogSettings.host = sharedDatabase.hostName();
            dialogSettings.port = sharedDatabase.port();
            dialogSettings.database = sharedDatabase.databaseName();
            dialogSettings.user = sharedDatabase.userName();
            dialogSettings.password = sharedDatabase.password();
            QSettings databaseSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
            dialogSettings.schema = databaseSettings.value(
                QStringLiteral("db/schema"), dialogSettings.schema.isEmpty() ? QStringLiteral("public") : dialogSettings.schema).toString();
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("连接 PostGIS 数据库"));
    dialog.resize(460, 500);
    dialog.setMinimumWidth(460);
    auto* layout = new QVBoxLayout(&dialog);
    auto* formLayout = new QFormLayout;
    auto* hostEdit = new QLineEdit(dialogSettings.host, &dialog);
    auto* portSpin = new QSpinBox(&dialog);
    portSpin->setRange(1, 65535);
    portSpin->setValue(dialogSettings.port > 0 ? dialogSettings.port : 5432);
    auto* databaseEdit = new QLineEdit(dialogSettings.database, &dialog);
    auto* schemaEdit = new QLineEdit(dialogSettings.schema.isEmpty() ? QStringLiteral("public") : dialogSettings.schema, &dialog);
    auto* userEdit = new QLineEdit(dialogSettings.user, &dialog);
    auto* passwordEdit = new QLineEdit(dialogSettings.password, &dialog);
    passwordEdit->setEchoMode(QLineEdit::Password);
    // 未选择“记住密码”时，密码不落盘；若下载页的共享会话仍在线，
    // 仅在本次预处理窗口内取用其内存中的密码，供后续 QGIS 图层导出使用。
    if (passwordEdit->text().isEmpty()
        && QSqlDatabase::contains(mapProductManagerSharedPostgisConnectionName()))
    {
        const QSqlDatabase sharedDatabase = QSqlDatabase::database(
            mapProductManagerSharedPostgisConnectionName(), false);
        if (sharedDatabase.isValid() && sharedDatabase.isOpen()
            && sharedDatabase.hostName() == dialogSettings.host
            && sharedDatabase.port() == dialogSettings.port
            && sharedDatabase.databaseName() == dialogSettings.database
            && sharedDatabase.userName() == dialogSettings.user)
        {
            passwordEdit->setText(sharedDatabase.password());
        }
    }
    auto* rememberPasswordCheck = new QCheckBox(tr("记住密码（仅保存在本机配置中）"), &dialog);
    rememberPasswordCheck->setChecked(m_rememberPostgisPassword);
    auto* scopeCombo = new QComboBox(&dialog);
    scopeCombo->addItem(tr("整个主题（该主题下全部内容）"), static_cast<int>(PostgisThemeScope::Theme));
    scopeCombo->addItem(tr("主题中的单个内容"), static_cast<int>(PostgisThemeScope::ThemeContent));
    scopeCombo->setCurrentIndex(m_postgisSource.themeScope == PostgisThemeScope::ThemeContent ? 1 : 0);
    auto* themeCombo = new QComboBox(&dialog);
    auto* contentCombo = new QComboBox(&dialog);
    auto* contentLabel = new QLabel(tr("主题内容:"), &dialog);
    auto* statusLabel = new QLabel(tr("点击“连接”后将自动读取主题。"), &dialog);
    statusLabel->setWordWrap(true);

    formLayout->addRow(tr("主机地址:"), hostEdit);
    formLayout->addRow(tr("端口:"), portSpin);
    formLayout->addRow(tr("数据库名:"), databaseEdit);
    formLayout->addRow(tr("Schema:"), schemaEdit);
    formLayout->addRow(tr("用户名:"), userEdit);
    formLayout->addRow(tr("密码:"), passwordEdit);
    layout->addLayout(formLayout);

    auto* reuseConnectionHint = new QLabel(&dialog);
    reuseConnectionHint->setWordWrap(true);
    reuseConnectionHint->setStyleSheet(
        QStringLiteral("QLabel { color: #19703a; background: #edf8ef; border: 1px solid #9bcaa4; "
                       "border-radius: 3px; padding: 6px; font-weight: bold; }"));
    reuseConnectionHint->hide();
    layout->addWidget(reuseConnectionHint);

    auto* connectionButtonsLayout = new QHBoxLayout;
    connectionButtonsLayout->addStretch();
    auto* connectButton = new QPushButton(tr("连接"), &dialog);
    auto* testButton = new QPushButton(tr("测试连接"), &dialog);
    connectionButtonsLayout->addWidget(connectButton);
    connectionButtonsLayout->addWidget(testButton);
    layout->addLayout(connectionButtonsLayout);
    auto* themeFormLayout = new QFormLayout;
    themeFormLayout->addRow(tr("处理范围:"), scopeCombo);
    themeFormLayout->addRow(tr("主题:"), themeCombo);
    themeFormLayout->addRow(contentLabel, contentCombo);
    layout->addLayout(themeFormLayout);
    layout->addWidget(rememberPasswordCheck);
    layout->addWidget(statusLabel);
    auto* confirmButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    confirmButtons->button(QDialogButtonBox::Ok)->setText(tr("确认"));
    layout->addWidget(confirmButtons);
    connect(confirmButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    struct ThemeItem { qint64 id; QString name; };
    QList<ThemeItem> dialogThemes;
    bool themesLoaded = false;
    QString loadedConnectionKey;
    auto sourceSettingsFromInputs = [&]() {
        PostgisSourceSettings settings;
        settings.host = hostEdit->text().trimmed();
        settings.port = portSpin->value();
        settings.database = databaseEdit->text().trimmed();
        settings.schema = schemaEdit->text().trimmed();
        settings.user = userEdit->text().trimmed();
        settings.password = passwordEdit->text();
        return settings;
    };
    auto sourceConnectionKey = [&](const PostgisSourceSettings& settings) {
        return QStringLiteral("%1|%2|%3|%4|%5")
            .arg(settings.host).arg(settings.port).arg(settings.database, settings.schema, settings.user);
    };
    auto invalidateLoadedThemes = [&]() {
        themesLoaded = false;
        loadedConnectionKey.clear();
        themeCombo->clear();
        contentCombo->clear();
        reuseConnectionHint->hide();
    };
    connect(hostEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { invalidateLoadedThemes(); });
    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [&](int) { invalidateLoadedThemes(); });
    connect(databaseEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { invalidateLoadedThemes(); });
    connect(schemaEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { invalidateLoadedThemes(); });
    connect(userEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { invalidateLoadedThemes(); });
    connect(passwordEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { invalidateLoadedThemes(); });
    auto loadContents = [&]() {
        contentCombo->clear();
        const PostgisSourceSettings settings = sourceSettingsFromInputs();
        if (themeCombo->currentIndex() < 0)
            return;
        QString error;
        {
            PostgisQueryConnection connection(settings, reinterpret_cast<quintptr>(&dialog));
            if (!connection.isOpen())
                error = connection.error();
            else
            {
                const QString table = QStringLiteral("\"%1\".\"theme_contents\"").arg(settings.schema);
                QSqlQuery query(connection.database());
                query.prepare(QStringLiteral("SELECT id, content_name FROM %1 WHERE theme_id = :theme_id ORDER BY id")
                                  .arg(table));
                query.bindValue(QStringLiteral(":theme_id"), themeCombo->currentData());
                if (!query.exec())
                    error = tr("无法读取 theme_contents 表：%1").arg(query.lastError().text());
                else
                {
                    while (query.next())
                        contentCombo->addItem(QStringLiteral("%1（ID: %2）")
                                                  .arg(query.value(1).toString()).arg(query.value(0).toLongLong()),
                                              query.value(0).toLongLong());
                }
            }
        }
        if (!error.isEmpty())
        {
            statusLabel->setText(error);
            statusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
            return;
        }
        const int savedContentIndex = contentCombo->findData(m_postgisSource.themeContentId);
        if (savedContentIndex >= 0)
            contentCombo->setCurrentIndex(savedContentIndex);
    };
    auto updateContentVisibility = [&]() {
        const bool isContentScope = scopeCombo->currentData().toInt()
            == static_cast<int>(PostgisThemeScope::ThemeContent);
        contentLabel->setVisible(isContentScope);
        contentCombo->setVisible(isContentScope);
    };
    updateContentVisibility();
    connect(scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [&](int) { updateContentVisibility(); });
    connect(themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [&](int) { if (themesLoaded) loadContents(); });
    auto loadThemes = [&]() {
        const PostgisSourceSettings settings = sourceSettingsFromInputs();
        static const QRegularExpression validIdentifier(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        if (settings.host.isEmpty() || settings.database.isEmpty() || settings.schema.isEmpty() || settings.user.isEmpty()
            || !validIdentifier.match(settings.schema).hasMatch())
        {
            statusLabel->setText(tr("请完整填写主机、数据库、Schema 和用户名，并确保 Schema 名称有效。"));
            statusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
            return;
        }
        QString error;
        QList<ThemeItem> themes;
        bool reusedDownloadConnection = false;
        {
            PostgisQueryConnection connection(settings, reinterpret_cast<quintptr>(&dialog));
            reusedDownloadConnection = connection.usesSharedConnection();
            if (!connection.isOpen())
                error = connection.error();
            else
            {
                QSqlQuery query(connection.database());
                const QString table = QStringLiteral("\"%1\".\"themes\"").arg(settings.schema);
                if (!query.exec(QStringLiteral("SELECT id, name FROM %1 ORDER BY id").arg(table)))
                    error = tr("无法读取 themes 表：%1").arg(query.lastError().text());
                else
                {
                    while (query.next())
                        themes.append({ query.value(0).toLongLong(), query.value(1).toString() });
                }
            }
        }
        if (error.isEmpty() && themes.isEmpty())
            error = tr("themes 表中没有可选主题。");
        if (!error.isEmpty())
        {
            themesLoaded = false;
            themeCombo->clear();
            contentCombo->clear();
            statusLabel->setText(error);
            statusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
            return;
        }
        dialogThemes = themes;
        themesLoaded = true;
        loadedConnectionKey = sourceConnectionKey(settings);
        themeCombo->clear();
        for (const ThemeItem& theme : dialogThemes)
            themeCombo->addItem(QStringLiteral("%1（ID: %2）").arg(theme.name).arg(theme.id), theme.id);
        const int savedThemeIndex = themeCombo->findData(m_postgisSource.themeId);
        if (savedThemeIndex >= 0)
            themeCombo->setCurrentIndex(savedThemeIndex);
        loadContents();
        statusLabel->setText(reusedDownloadConnection
            ? tr("已复用数据库连接，读取 %1 个主题，请选择处理范围。")
                  .arg(dialogThemes.size())
            : tr("已读取 %1 个主题，请选择处理范围。").arg(dialogThemes.size()));
        statusLabel->setStyleSheet(QStringLiteral("color: #19703a;"));
        if (reusedDownloadConnection)
        {
            reuseConnectionHint->setText(
                tr("✓ 已复用“数据库连接”配置，无需再次连接；请选择处理范围和主题后确认。"));
            reuseConnectionHint->show();
        }
    };
    connect(connectButton, &QPushButton::clicked, &dialog, loadThemes);
    connect(confirmButtons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const PostgisSourceSettings settings = sourceSettingsFromInputs();
        if (!themesLoaded || loadedConnectionKey != sourceConnectionKey(settings) || themeCombo->currentIndex() < 0)
        {
            statusLabel->setText(tr("请先点击“连接”并选择主题。"));
            statusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
            return;
        }
        if (scopeCombo->currentData().toInt() == static_cast<int>(PostgisThemeScope::ThemeContent)
            && contentCombo->currentIndex() < 0)
        {
            statusLabel->setText(tr("请选择主题内容，或将处理范围改为“整个主题”。"));
            statusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
            return;
        }
        dialog.accept();
    });

    connect(testButton, &QPushButton::clicked, &dialog, [&]() {
        const PostgisSourceSettings settings = sourceSettingsFromInputs();
        QString error;
        {
            PostgisQueryConnection connection(settings, reinterpret_cast<quintptr>(this));
            if (!connection.isOpen())
                error = connection.error();
            else
            {
                QSqlQuery query(connection.database());
                if (!query.exec(QStringLiteral("SELECT PostGIS_Version()")) || !query.next())
                    error = tr("数据库已连接，但未检测到 PostGIS：%1").arg(query.lastError().text());
            }
        }
        if (error.isEmpty())
        {
            statusLabel->setText(tr("测试连接成功，已检测到 PostGIS。"));
            statusLabel->setStyleSheet(QStringLiteral("color: #19703a;"));
        }
        else
        {
            statusLabel->setText(tr("连接失败：%1").arg(error));
            statusLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
        }
    });

    // 数据库连接已连接到同一服务器时，直接读取主题，无需用户再次点击“连接”。
    const PostgisSourceSettings initialSettings = sourceSettingsFromInputs();
    if (QSqlDatabase::contains(mapProductManagerSharedPostgisConnectionName()))
    {
        const QSqlDatabase sharedDatabase = QSqlDatabase::database(
            mapProductManagerSharedPostgisConnectionName(), false);
        if (sharedDatabase.isValid() && sharedDatabase.isOpen()
            && sharedDatabase.hostName() == initialSettings.host
            && sharedDatabase.port() == initialSettings.port
            && sharedDatabase.databaseName() == initialSettings.database
            && sharedDatabase.userName() == initialSettings.user)
        {
            loadThemes();
        }
    }

    if (dialog.exec() != QDialog::Accepted)
        return;

    PostgisSourceSettings settings;
    settings.host = hostEdit->text().trimmed();
    settings.port = portSpin->value();
    settings.database = databaseEdit->text().trimmed();
    settings.schema = schemaEdit->text().trimmed();
    settings.user = userEdit->text().trimmed();
    settings.password = passwordEdit->text();
    if (settings.host.isEmpty() || settings.database.isEmpty() || settings.schema.isEmpty() || settings.user.isEmpty())
    {
        QMessageBox::warning(this, tr("参数错误"), tr("请完整填写主机、数据库、Schema 和用户名。"));
        return;
    }

    const PostgisSourceSettings previousSettings = m_postgisSource;
    const bool previousRememberPassword = m_rememberPostgisPassword;
    m_postgisSource = settings;
    m_rememberPostgisPassword = rememberPasswordCheck->isChecked();
    QString error;
    m_postgisSource.themeScope = static_cast<PostgisThemeScope>(scopeCombo->currentData().toInt());
    m_postgisSource.themeId = themeCombo->currentData().toLongLong();
    m_postgisSource.themeName = dialogThemes.at(themeCombo->currentIndex()).name;
    if (m_postgisSource.themeScope == PostgisThemeScope::ThemeContent)
    {
        m_postgisSource.themeContentId = contentCombo->currentData().toLongLong();
        m_postgisSource.themeContentName = contentCombo->currentText().section(QChar(0xFF08), 0, 0);
    }
    if (!loadPostgisSpatialLayers(&error))
    {
        m_postgisSource = previousSettings;
        m_rememberPostgisPassword = previousRememberPassword;
        updateSourceDataUi();
        QMessageBox::warning(this, tr("连接失败"), error);
        return;
    }

    m_postgisSource.layers = m_availablePostgisLayers;
    updateSourceDataUi();
    QMessageBox::information(this, tr("连接成功"),
                             tr("已连接 PostGIS。处理时将裁剪%1。")
                                 .arg(postgisLayersDisplayText()));
}

bool CSE_PreprocessDialog::loadPostgisSpatialLayers(QString* errorMessage)
{
    m_availablePostgisLayers.clear();
    if (m_postgisSource.themeId <= 0)
    {
        if (errorMessage)
            *errorMessage = tr("请先选择要处理的主题。");
        return false;
    }
    static const QRegularExpression validIdentifier(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    if (!validIdentifier.match(m_postgisSource.schema).hasMatch())
    {
        if (errorMessage)
            *errorMessage = tr("Schema 名称无效。");
        return false;
    }
    QString error;
    {
        PostgisQueryConnection connection(m_postgisSource, reinterpret_cast<quintptr>(this));
        if (!connection.isOpen())
        {
            error = connection.error();
        }
        else
        {
            const QString schema = m_postgisSource.schema;
            const QString contentsTable = QStringLiteral("\"%1\".\"theme_contents\"").arg(schema);
            const QString metadataTable = QStringLiteral("\"%1\".\"product_metadata\"").arg(schema);
            QSet<QString> selectedTableNames;
            QSqlQuery productQuery(connection.database());
            QString productSql = QStringLiteral(
                "SELECT DISTINCT pm.layer_table_name "
                "FROM %1 tc "
                "CROSS JOIN LATERAL regexp_split_to_table(COALESCE(tc.product_ids, ''), ',') AS product_id(value) "
                "JOIN %2 pm ON pm.id = CASE "
                "WHEN btrim(product_id.value) ~ '^[0-9]+$' THEN btrim(product_id.value)::bigint "
                "ELSE NULL END "
                "WHERE tc.theme_id = :theme_id "
                "AND pm.layer_table_name IS NOT NULL AND btrim(pm.layer_table_name) <> ''")
                .arg(contentsTable, metadataTable);
            if (m_postgisSource.themeScope == PostgisThemeScope::ThemeContent)
                productSql += QStringLiteral(" AND tc.id = :theme_content_id");
            productQuery.prepare(productSql);
            productQuery.bindValue(QStringLiteral(":theme_id"), m_postgisSource.themeId);
            if (m_postgisSource.themeScope == PostgisThemeScope::ThemeContent)
                productQuery.bindValue(QStringLiteral(":theme_content_id"), m_postgisSource.themeContentId);
            if (!productQuery.exec())
            {
                error = tr("无法根据主题内容读取成果图层：%1").arg(productQuery.lastError().text());
            }
            else
            {
                while (productQuery.next())
                    selectedTableNames.insert(productQuery.value(0).toString());
            }

            if (error.isEmpty() && selectedTableNames.isEmpty())
            {
                error = m_postgisSource.themeScope == PostgisThemeScope::ThemeContent
                    ? tr("主题内容“%1”未关联有效成果（product_ids）。")
                          .arg(m_postgisSource.themeContentName)
                    : tr("主题“%1”下的内容未关联有效成果（product_ids）。")
                          .arg(m_postgisSource.themeName);
            }

            QSqlQuery query(connection.database());
            if (!error.isEmpty())
            {
                // 主题关联不正确时不再回退到整个 Schema，避免误处理无关数据。
            }
            else
            {
            query.prepare(QStringLiteral(
                "SELECT f_table_name, f_geometry_column "
                "FROM public.geometry_columns "
                "WHERE f_table_schema = :schema "
                "ORDER BY f_table_name, f_geometry_column"));
            query.bindValue(QStringLiteral(":schema"), m_postgisSource.schema);
            bool queryOk = query.exec();
            if (!queryOk)
            {
                // 兼容未注册 geometry_columns 视图的 PostGIS 数据库。
                query.prepare(QStringLiteral(
                    "SELECT table_name, column_name "
                    "FROM information_schema.columns "
                    "WHERE table_schema = :schema AND udt_name IN ('geometry', 'geography') "
                    "ORDER BY table_name, column_name"));
                query.bindValue(QStringLiteral(":schema"), m_postgisSource.schema);
                queryOk = query.exec();
            }
            if (!queryOk)
            {
                error = query.lastError().text();
            }
            else
            {
                while (query.next())
                {
                    const QString tableName = query.value(0).toString();
                    if (selectedTableNames.contains(tableName))
                        m_availablePostgisLayers.append({ tableName, query.value(1).toString() });
                }
            }
            }
        }
    }
    if (!error.isEmpty())
    {
        if (errorMessage)
            *errorMessage = error;
        return false;
    }
    if (m_availablePostgisLayers.isEmpty())
    {
        if (errorMessage)
            *errorMessage = tr("所选主题范围未找到可用的 PostGIS 空间图层。");
        return false;
    }
    return true;
}

// 符号库相关槽函数实现
void CSE_PreprocessDialog::onAddSymbolPDFClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("选择符号库PDF文件"),
        QString(),
        tr("PDF文件 (*.pdf)"));

    if (!files.isEmpty())
    {
        for (const QString &file : files)
        {
            if (ui.listWidget_SymbolPDFPaths->findItems(file, Qt::MatchExactly).isEmpty())
            {
                ui.listWidget_SymbolPDFPaths->addItem(file);
            }
        }
    }
}

void CSE_PreprocessDialog::onRemoveSymbolPDFClicked()
{
    int currentRow = ui.listWidget_SymbolPDFPaths->currentRow();
    if (currentRow >= 0)
    {
        delete ui.listWidget_SymbolPDFPaths->takeItem(currentRow);
    }
}

void CSE_PreprocessDialog::onClearSymbolPDFClicked()
{
    ui.listWidget_SymbolPDFPaths->clear();
}

void CSE_PreprocessDialog::onBrowseSymbolOutputClicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("选择符号库输出目录"),
        ui.lineEdit_SymbolOutputPath->text());

    if (!dir.isEmpty())
    {
        ui.lineEdit_SymbolOutputPath->setText(dir);
    }
}

void CSE_PreprocessDialog::onStartProcessClicked()
{
    if (!validateInputParams())
    {
        return;
    }

    // 开始处理
    updateUIState(true);
    ui.progressBar->setValue(0);
    ui.textEdit_Log->clear();
    m_failedSteps = 0;
    m_errorMessages.clear();
    m_cancelRequested = false;

    appendLog(tr("开始预处理..."));

    // 计算总步骤数
    m_currentStep = 0;
    m_totalSteps = 0;

    // 判断当前是哪个标签页
    int currentTab = ui.tabWidget->currentIndex();

    if (currentTab == 0) // 地图预处理
    {
        m_isSymbolProcessing = false;
        m_totalSteps = ui.listWidget_PDFPaths->count() * mapStepsPerPdf();
    }
    else // 符号库预处理
    {
        m_isSymbolProcessing = true;
        // 多个标准图例PDF合并解析为一套标准图例表，因此这里只需要一个步骤。
        m_totalSteps = 1;
    }

    // 开始第一个处理步骤
    processNextStep();
}

void CSE_PreprocessDialog::processNextStep()
{
    // 更新进度
    if (m_totalSteps > 0)
    {
        int progress = (m_currentStep * 100) / m_totalSteps;
        ui.progressBar->setValue(progress);
    }

    // 检查是否完成所有步骤
    if (m_currentStep >= m_totalSteps)
    {
        appendLog(tr("\n========== 处理完成 =========="));

        int totalItems = m_isSymbolProcessing
                             ? ui.listWidget_SymbolPDFPaths->count()
                             : ui.listWidget_PDFPaths->count();

        int successItems = 0;
        if (m_isSymbolProcessing)
        {
            // 标准图例PDF是合并解析的：任一步失败就表示这批PDF没有生成完整标准表。
            successItems = (m_failedSteps == 0) ? totalItems : 0;
        }
        else
        {
            // 任一步失败都会跳过该 PDF 的后续步骤，因此每个失败步骤对应一幅失败地图。
            successItems = totalItems - std::min(totalItems, m_failedSteps);
        }

        appendLog(tr("总计: %1 个文件，成功: %2，失败: %3")
                      .arg(totalItems)
                      .arg(successItems)
                      .arg(m_failedSteps > 0 ? totalItems - successItems : 0));

        if (m_failedSteps > 0)
        {
            appendLog(tr("\n失败步骤数: %1").arg(m_failedSteps));
            appendLog(tr("详细错误信息:"));
            for (const QString &msg : m_errorMessages)
            {
                appendLog(tr("  - %1").arg(msg));
            }

            QMessageBox::warning(this, tr("处理完成(含错误)"),
                                 tr("处理已完成，但有 %1 个步骤失败。\n请查看日志了解详情。").arg(m_failedSteps));
        }
        else
        {
            appendLog(tr("✓ 所有步骤执行成功！"));
            QMessageBox::information(this, tr("处理完成"),
                                     tr("所有 %1 个文件已成功处理！").arg(totalItems));
        }

        updateUIState(false);
        ui.progressBar->setValue(100);
        return;
    }

    if (m_isSymbolProcessing)
    {
        appendLog(QString::fromUtf8(u8"[符号库] 解析 %1 个标准图例PDF...")
                      .arg(ui.listWidget_SymbolPDFPaths->count()));
        executeStandardLegendPdfPreprocess();
        return;
    }
    else
    {
        // 新的处理逻辑：按PDF逐个完成所有步骤
        const int stepsPerPDF = mapStepsPerPdf();

        if (stepsPerPDF == 0) // 理论上不会发生，因为至少有2步
        {
            appendLog(tr("没有选择任何处理步骤,处理结束。"));
            updateUIState(false);
            ui.progressBar->setValue(100);
            return;
        }

        int pdfIndex = m_currentStep / stepsPerPDF;
        int stepInPDF = m_currentStep % stepsPerPDF;
        QString pdfName = QFileInfo(ui.listWidget_PDFPaths->item(pdfIndex)->text()).baseName();
        int totalPDFs = ui.listWidget_PDFPaths->count();

        int currentStepInLogic = 0;

        // 步骤1: PDF -> GeoJSON (固定执行)
        if (stepInPDF == currentStepInLogic)
        {
            appendLog(tr("[PDF %1/%2] 步骤 1/%3: [%4] 转换为GeoJSON...")
                          .arg(pdfIndex + 1)
                          .arg(totalPDFs)
                          .arg(stepsPerPDF)
                          .arg(pdfName));
            executeLaunchAIForSinglePDF(pdfIndex);
            return;
        }
        currentStepInLogic++;

        // 步骤2: GeoJSON -> SHP (固定执行)
        if (stepInPDF == currentStepInLogic)
        {
            appendLog(tr("[PDF %1/%2] 步骤 2/%3: [%4] GeoJSON转换为Shapefile...")
                          .arg(pdfIndex + 1)
                          .arg(totalPDFs)
                          .arg(stepsPerPDF)
                          .arg(pdfName));
            executeGeoJSON2Shp(pdfIndex);
            return;
        }
        currentStepInLogic++;

        // 步骤3: 源矢量数据裁剪 (可选)
        if (hasSourceData())
        {
            if (stepInPDF == currentStepInLogic)
            {
                const bool isPostgis = currentSourceDataType() == SourceDataType::Postgis;
                appendLog(tr("[PDF %1/%2] 步骤 3/%3: [%4] 根据范围裁剪%5数据...")
                              .arg(pdfIndex + 1)
                              .arg(totalPDFs)
                              .arg(stepsPerPDF)
                              .arg(pdfName)
                              .arg(isPostgis ? tr("PostGIS") : tr("GDB")));
                if (isPostgis)
                    executePostgis2Shp(pdfIndex);
                else
                    executeGDB2Shp(pdfIndex);
                return;
            }
            currentStepInLogic++;
        }
    }

    // 注意：此处的 processNextStep() 调用已移至 onProcessFinished 槽函数中
    // 以确保一个步骤完成后再开始下一个
}

void CSE_PreprocessDialog::onCancelClicked()
{
    m_cancelRequested = true;
    if (m_pCurrentProcess && m_pCurrentProcess->state() != QProcess::NotRunning)
    {
        appendLog(tr("用户取消处理..."));
        m_pCurrentProcess->kill();
        m_pCurrentProcess->waitForFinished(3000);
    }
    updateUIState(false);
}

void CSE_PreprocessDialog::onCloseClicked()
{
    this->close();
}

void CSE_PreprocessDialog::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QString status = (exitStatus == QProcess::NormalExit) ? tr("正常退出") : tr("异常崩溃");
    appendLog(tr("脚本执行完毕。退出码: %1, 状态: %2").arg(exitCode).arg(status));

    if (exitCode == 0 && exitStatus == QProcess::NormalExit)
    {
        // GeoJSON 转换为 SHP 成功后，将对应 GeoPDF 放入输出根目录。
        if (!m_isSymbolProcessing)
        {
            const int stepsPerPDF = mapStepsPerPdf();
            int stepInPDF = m_currentStep % stepsPerPDF;
            int pdfIndex = m_currentStep / stepsPerPDF;
            if (stepInPDF == 1)
            {
                copyGeoPdfToOutputRoot(pdfIndex);
            }
        }

        // 成功，继续下一步
        appendLog(tr("✓ 步骤执行成功"));
        m_currentStep++;
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
    }
    else
    {
        // 失败，记录错误但继续处理
        m_failedSteps++;

        // 获取当前处理的文件名
        QString currentFile;
        if (m_isSymbolProcessing)
        {
            currentFile = QString::fromUtf8(u8"标准图例PDF预处理");
        }
        else
        {
            const int stepsPerPDF = mapStepsPerPdf();
            int pdfIndex = m_currentStep / stepsPerPDF;
            if (pdfIndex < ui.listWidget_PDFPaths->count())
            {
                currentFile = QFileInfo(ui.listWidget_PDFPaths->item(pdfIndex)->text()).baseName();
            }
        }

        QString errorMsg = tr("✗ 当前步骤执行失败 [%1]，跳过并继续...").arg(currentFile);
        appendLog(errorMsg);
        m_errorMessages.append(errorMsg);

        // 判断是否需要跳过当前PDF/符号库的剩余步骤
        if (m_isSymbolProcessing)
        {
            m_currentStep = m_totalSteps;
        }
        else
        {
            // 地图处理：跳到下一个PDF的第一步
            const int stepsPerPDF = mapStepsPerPdf();

            int pdfIndex = m_currentStep / stepsPerPDF;
            int nextPdfFirstStep = (pdfIndex + 1) * stepsPerPDF;

            if (nextPdfFirstStep < m_totalSteps)
            {
                appendLog(tr("→ 跳过当前PDF的剩余步骤，处理下一个PDF"));
                m_currentStep = nextPdfFirstStep;
            }
            else
            {
                // 已是最后一个PDF，直接结束
                m_currentStep = m_totalSteps;
            }
        }

        // 继续处理
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
    }
}

void CSE_PreprocessDialog::onProcessReadyReadStandardOutput()
{
    if (m_pCurrentProcess)
    {
        QByteArray output = m_pCurrentProcess->readAllStandardOutput();
        QString text = QString::fromUtf8(output);
        if (text.isEmpty()) // 回退兼容本地编码
            text = QString::fromLocal8Bit(output);
        appendLog(text.trimmed());
    }
}

void CSE_PreprocessDialog::onProcessReadyReadStandardError()
{
    if (m_pCurrentProcess)
    {
        QByteArray error = m_pCurrentProcess->readAllStandardError();
        QString text = QString::fromUtf8(error);
        if (text.isEmpty())
            text = QString::fromLocal8Bit(error);
        appendLog(tr("错误输出: ") + text.trimmed());
    }
}

void CSE_PreprocessDialog::appendLog(const QString &message)
{
    if (message.isEmpty())
        return;
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    ui.textEdit_Log->append(QString("[%1] %2").arg(timestamp, message));
}

QString CSE_PreprocessDialog::getQgisPythonPath()
{
    // 尝试从QGIS设置中获取Python路径
    QString pythonPath = QgsApplication::prefixPath() + "/python.exe";
    if (QFileInfo::exists(pythonPath))
    {
        return pythonPath;
    }
    // 如果QGIS内置的找不到，返回一个通用的名称，让系统在PATH中查找
    return "python.exe";
}

QString CSE_PreprocessDialog::getStandardLegendPythonPath()
{
    QStringList candidates;
    QString bundledPackageDir = QFileInfo(getScriptFullPath("extract_standard_legend_pdf.py")).absoluteDir().filePath("python_packages");

    const QString qgisPython = getQgisPythonPath();
    candidates << qgisPython;

    QByteArray prefix = qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01");
    QString baseDir = QString::fromUtf8(prefix);
    if (!baseDir.isEmpty())
    {
        candidates << QDir(baseDir).filePath("MPQIS/apps/Python39/python.exe");
        candidates << QDir(baseDir).filePath("MPQIS/bin/python.exe");
    }

    candidates << "python.exe";

    QStringList checked;
    for (const QString &candidate : candidates)
    {
        if (candidate.isEmpty() || checked.contains(candidate, Qt::CaseInsensitive))
            continue;
        checked << candidate;

        if (candidate.endsWith(".exe", Qt::CaseInsensitive) && !QFileInfo::exists(candidate))
            continue;

        QProcess probe;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PYTHONIOENCODING", "utf-8");
        env.insert("PYTHONDONTWRITEBYTECODE", "1");
        if (QDir(bundledPackageDir).exists())
        {
            QString oldPythonPath = env.value("PYTHONPATH");
            env.insert("PYTHONPATH", oldPythonPath.isEmpty()
                                         ? bundledPackageDir
                                         : bundledPackageDir + QDir::listSeparator() + oldPythonPath);
        }
        probe.setProcessEnvironment(env);
        probe.start(candidate, QStringList() << "-c" << "import fitz");
        if (!probe.waitForFinished(5000))
        {
            probe.kill();
            probe.waitForFinished(1000);
            continue;
        }

        if (probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0)
        {
            return candidate;
        }
    }

    // 没找到 fitz 时仍返回QGIS Python，让脚本报出明确的缺依赖错误。
    return qgisPython;
}

QString CSE_PreprocessDialog::getScriptFullPath(const QString &scriptName)
{
    // 先尝试从环境变量读取安装前缀
    QByteArray prefix = qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01");
    QString baseDir = QString::fromUtf8(prefix);

    if (!baseDir.isEmpty())
    {
        // 环境变量存在：使用 MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01/preprocess_scripts/<scriptName>
        QString scriptDir = QDir(baseDir).filePath("MPQIS/preprocess_scripts");
        return QDir(scriptDir).filePath(scriptName);
    }

    // 如果没读到环境变量则返回空字符串
    return QString();
}

void CSE_PreprocessDialog::startPythonScript(const QString &scriptPath, const QStringList &arguments)
{
    // 进程已在构造函数中创建并连接好信号，此处无需再操作
    QString pythonExecutable = getQgisPythonPath();

    // 强制 Python 使用 UTF-8 输出，避免 stdout/stderr 中文乱码
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONIOENCODING", "utf-8");
    m_pCurrentProcess->setProcessEnvironment(env);
    // 预处理脚本中的 JSX 等资源使用相对路径，工作目录必须是脚本所在目录。
    m_pCurrentProcess->setWorkingDirectory(QFileInfo(scriptPath).absolutePath());

    QStringList processArgs;
    processArgs << scriptPath;
    processArgs.append(arguments);

    appendLog(tr("执行命令: %1 %2").arg(pythonExecutable, processArgs.join(" ")));
    m_pCurrentProcess->start(pythonExecutable, processArgs);
}

void CSE_PreprocessDialog::executeLaunchAIForSinglePDF(int pdfIndex)
{
    QString scriptPath = getScriptFullPath("launch_ai2.py");
    if (!QFileInfo::exists(scriptPath))
    {
        appendLog(tr("错误: 找不到 launch_ai2.py 脚本文件"));
        m_currentStep++;
        processNextStep();
        return;
    }

    QString pdfPath = ui.listWidget_PDFPaths->item(pdfIndex)->text();
    QStringList args;
    args << "--pdf-file" << pdfPath;
    args << "--output-dir" << ui.lineEdit_PDFOutputPath->text();

    startPythonScript(scriptPath, args);
}

void CSE_PreprocessDialog::executeGeoJSON2Shp(int pdfIndex)
{
    QString scriptPath = getScriptFullPath("geojson2shp2.py");
    if (!QFileInfo::exists(scriptPath))
    {
        appendLog(tr("错误: 找不到 geojson2shp2.py 脚本文件"));
        m_currentStep++;
        processNextStep();
        return;
    }

    QString pdfPath = ui.listWidget_PDFPaths->item(pdfIndex)->text();
    QFileInfo pdfInfo(pdfPath);
    QString baseName = pdfInfo.baseName();
    QString geojsonFolder = QDir(ui.lineEdit_PDFOutputPath->text()).filePath(baseName + "_geojson");
    QString outputFolder = QDir(ui.lineEdit_PDFOutputPath->text()).filePath(baseName + "_shp");

    QStringList args;
    args << "--geopdf" << pdfPath; // 添加这一行
    args << "--geojson-folder" << geojsonFolder;
    args << "--output-folder" << outputFolder;

    startPythonScript(scriptPath, args);
}

bool CSE_PreprocessDialog::copyGeoPdfToOutputRoot(int pdfIndex)
{
    if (pdfIndex < 0 || pdfIndex >= ui.listWidget_PDFPaths->count())
    {
        appendLog(tr("警告: GeoPDF 索引无效，无法复制原始文件"));
        return false;
    }

    const QString sourcePdfPath = ui.listWidget_PDFPaths->item(pdfIndex)->text();
    QFileInfo sourceInfo(sourcePdfPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile())
    {
        appendLog(tr("警告: 找不到原始 GeoPDF，无法复制: %1").arg(sourcePdfPath));
        return false;
    }

    const QString outputRoot = ui.lineEdit_PDFOutputPath->text();
    if (!QDir().mkpath(outputRoot))
    {
        appendLog(tr("警告: 无法创建 GeoPDF 输出目录: %1").arg(outputRoot));
        return false;
    }

    const QString targetPdfPath = QDir(outputRoot).filePath(sourceInfo.fileName());
    if (QFileInfo::exists(targetPdfPath) && !QFile::remove(targetPdfPath))
    {
        appendLog(tr("警告: 无法覆盖已有 GeoPDF: %1").arg(targetPdfPath));
        return false;
    }

    if (!QFile::copy(sourcePdfPath, targetPdfPath))
    {
        appendLog(tr("警告: GeoPDF 复制失败: %1 -> %2").arg(sourcePdfPath, targetPdfPath));
        return false;
    }

    appendLog(tr("✓ 已复制 GeoPDF 到输出目录: %1").arg(targetPdfPath));
    return true;
}

void CSE_PreprocessDialog::executeGDB2Shp(int pdfIndex)
{
    QString scriptPath = getScriptFullPath("gdb2shp_by_pdf.py");
    if (!QFileInfo::exists(scriptPath))
    {
        appendLog(tr("错误: 找不到 gdb2shp_by_pdf.py 脚本文件"));
        m_currentStep++;
        processNextStep();
        return;
    }

    QString pdfPath = ui.listWidget_PDFPaths->item(pdfIndex)->text();
    QFileInfo pdfInfo(pdfPath);
    QString baseName = pdfInfo.baseName();
    QString shpFolder = QDir(ui.lineEdit_PDFOutputPath->text()).filePath(baseName + "_shp");
    QString frameShpPath = QDir(shpFolder).filePath("内图廓.shp");

    QStringList args;
    args << "--source-gdb" << ui.lineEdit_SourceGDB->text();
    args << "--frame-shp" << frameShpPath;
    args << "--output-dir" << ui.lineEdit_PDFOutputPath->text();
    args << "--output-name" << baseName;

    startPythonScript(scriptPath, args);
}

void CSE_PreprocessDialog::executePostgis2Shp(int pdfIndex)
{
    if (pdfIndex < 0 || pdfIndex >= ui.listWidget_PDFPaths->count())
    {
        completeInternalSourceStep(false, tr("GeoPDF 索引无效，无法执行 PostGIS 裁剪。"));
        return;
    }

    const QString baseName = QFileInfo(ui.listWidget_PDFPaths->item(pdfIndex)->text()).baseName();
    const QString frameShpPath = QDir(ui.lineEdit_PDFOutputPath->text())
        .filePath(baseName + QStringLiteral("_shp/内图廓.shp"));
    QString error;
    const bool success = clipPostgisLayersByFrame(frameShpPath, ui.lineEdit_PDFOutputPath->text(), baseName, &error);
    if (m_cancelRequested)
    {
        appendLog(tr("已取消 PostGIS 裁剪。"));
        return;
    }
    completeInternalSourceStep(success, error);
}

bool CSE_PreprocessDialog::clipPostgisLayersByFrame(const QString& frameShpPath, const QString& outputRoot,
                                                    const QString& pdfBaseName, QString* errorMessage)
{
    if (!QFileInfo::exists(frameShpPath))
    {
        if (errorMessage)
            *errorMessage = tr("未找到内图廓文件：%1").arg(frameShpPath);
        return false;
    }

    QgsVectorLayer frameLayer(frameShpPath, QStringLiteral("frame"), QStringLiteral("ogr"));
    if (!frameLayer.isValid())
    {
        if (errorMessage)
            *errorMessage = tr("无法读取内图廓文件：%1").arg(frameShpPath);
        return false;
    }

    QgsGeometry frameGeometry;
    QgsFeature frameFeature;
    QgsFeatureIterator frameIterator = frameLayer.getFeatures();
    while (frameIterator.nextFeature(frameFeature))
    {
        if (!frameFeature.hasGeometry())
            continue;
        const QgsGeometry frameArea = frameGeometryToArea(frameFeature.geometry());
        if (frameArea.isNull() || frameArea.isEmpty())
            continue;
        frameGeometry = frameGeometry.isNull()
            ? frameArea
            : frameGeometry.combine(frameArea);
    }
    if (frameGeometry.isNull() || frameGeometry.isEmpty())
    {
        if (errorMessage)
            *errorMessage = tr("内图廓文件中没有可用的几何范围。");
        return false;
    }
    if (QgsWkbTypes::geometryType(frameLayer.wkbType()) == QgsWkbTypes::LineGeometry)
        appendLog(tr("  [INFO] 已将线状内图廓转换为面范围后裁剪 PostGIS 图层。"));

    const QString destinationDir = QDir(outputRoot).filePath(pdfBaseName + QStringLiteral("_gdb_shp"));
    if (!QDir().mkpath(destinationDir))
    {
        if (errorMessage)
            *errorMessage = tr("无法创建输出目录：%1").arg(destinationDir);
        return false;
    }

    int loadedLayerCount = 0;
    int layersWithFeatures = 0;
    int skippedLayerCount = 0;
    for (const PostgisLayerInfo& layerInfo : m_postgisSource.layers)
    {
        if (m_cancelRequested)
            return false;

        QgsDataSourceUri uri;
        uri.setConnection(m_postgisSource.host, QString::number(m_postgisSource.port),
                          m_postgisSource.database, m_postgisSource.user, m_postgisSource.password);
        uri.setDataSource(m_postgisSource.schema, layerInfo.tableName, layerInfo.geometryColumn,
                          QString(), QString());
        uri.setUseEstimatedMetadata(true);
        QgsVectorLayer sourceLayer(uri.uri(), layerInfo.tableName, QStringLiteral("YOUR_PASSWORD"));
        if (!sourceLayer.isValid())
        {
            // 部分 QGIS Postgres provider 在图层无效时不保证 dataProvider() 可安全访问。
            // 此处不读取 provider 的详细错误，避免无效图层导致插件崩溃。
            appendLog(tr("  [WARN] 跳过无法加载的 PostGIS 图层：%1.%2")
                          .arg(m_postgisSource.schema, layerInfo.tableName));
            ++skippedLayerCount;
            continue;
        }
        ++loadedLayerCount;

        QgsGeometry sourceFrameGeometry = frameGeometry;
        if (frameLayer.crs().isValid() && sourceLayer.crs().isValid() && frameLayer.crs() != sourceLayer.crs())
        {
            QgsCoordinateTransform transform(frameLayer.crs(), sourceLayer.crs(), QgsProject::instance());
            if (sourceFrameGeometry.transform(transform) != Qgis::GeometryOperationResult::Success)
            {
                if (errorMessage)
                    *errorMessage = tr("无法将内图廓坐标转换到图层 %1 的坐标系。").arg(layerInfo.tableName);
                return false;
            }
        }

        const QString geometryType = QgsWkbTypes::displayString(sourceLayer.wkbType());
        QString memoryUri = geometryType;
        if (sourceLayer.crs().isValid())
            memoryUri += QStringLiteral("?crs=") + sourceLayer.crs().authid();
        const QString outputLayerName = outputLayerNameFromPostgisTable(layerInfo.tableName, pdfBaseName);
        QgsVectorLayer outputLayer(memoryUri, outputLayerName, QStringLiteral("memory"));
        if (!outputLayer.isValid())
        {
            if (errorMessage)
                *errorMessage = tr("无法创建图层 %1 的临时输出数据。" ).arg(layerInfo.tableName);
            return false;
        }

        QList<QgsField> outputFields;
        const QgsFields sourceFields = sourceLayer.fields();
        for (int index = 0; index < sourceFields.count(); ++index)
        {
            QgsField field = sourceFields.at(index);
            if (field.type() == QVariant::Double)
            {
                field.setType(QVariant::String);
                field.setTypeName(QStringLiteral("String"));
                field.setLength(0);
                field.setPrecision(0);
            }
            outputFields.append(field);
        }
        if (!outputLayer.dataProvider()->addAttributes(outputFields))
        {
            if (errorMessage)
                *errorMessage = tr("无法创建图层 %1 的属性字段。" ).arg(layerInfo.tableName);
            return false;
        }
        outputLayer.updateFields();

        QgsFeatureRequest request;
        request.setFilterRect(sourceFrameGeometry.boundingBox());
        QgsFeatureIterator sourceIterator = sourceLayer.getFeatures(request);
        QgsFeature sourceFeature;
        QgsFeatureList outputFeatures;
        int featureCount = 0;
        while (sourceIterator.nextFeature(sourceFeature))
        {
            QApplication::processEvents();
            if (m_cancelRequested)
                return false;
            if (!sourceFeature.hasGeometry() || !sourceFeature.geometry().intersects(sourceFrameGeometry))
                continue;

            QgsGeometry clippedGeometry = sourceFeature.geometry().intersection(sourceFrameGeometry);
            if (clippedGeometry.isNull() || clippedGeometry.isEmpty())
                continue;
            if (QgsWkbTypes::geometryType(clippedGeometry.wkbType())
                != QgsWkbTypes::geometryType(sourceLayer.wkbType()))
                continue;

            QgsFeature outputFeature(outputLayer.fields());
            QgsAttributes attributes = sourceFeature.attributes();
            for (int index = 0; index < attributes.size(); ++index)
            {
                if (index < sourceLayer.fields().count()
                    && sourceLayer.fields().at(index).type() == QVariant::Double
                    && !attributes.at(index).isNull())
                {
                    attributes[index] = QString::number(attributes.at(index).toDouble(), 'g', 15);
                }
            }
            outputFeature.setAttributes(attributes);
            outputFeature.setGeometry(clippedGeometry);
            outputFeatures.append(outputFeature);
            ++featureCount;

            if (outputFeatures.size() >= 500)
            {
                if (!outputLayer.dataProvider()->addFeatures(outputFeatures))
                {
                    if (errorMessage)
                        *errorMessage = tr("写入图层 %1 的裁剪要素失败。" ).arg(layerInfo.tableName);
                    return false;
                }
                outputFeatures.clear();
            }
        }
        if (!outputFeatures.isEmpty() && !outputLayer.dataProvider()->addFeatures(outputFeatures))
        {
            if (errorMessage)
                *errorMessage = tr("写入图层 %1 的裁剪要素失败。" ).arg(layerInfo.tableName);
            return false;
        }
        outputLayer.updateExtents();

        const QString outputShp = QDir(destinationDir).filePath(
            outputLayerName + QStringLiteral(".shp"));
        if (featureCount == 0)
        {
            // 同一输出目录可能来自上一次运行；当前图层没有裁剪结果时不能保留旧文件。
            const QString outputBase = QFileInfo(outputShp).absolutePath()
                + QDir::separator() + QFileInfo(outputShp).completeBaseName();
            const QStringList sidecarExtensions = {
                QStringLiteral(".shp"), QStringLiteral(".shx"), QStringLiteral(".dbf"),
                QStringLiteral(".prj"), QStringLiteral(".cpg"), QStringLiteral(".qpj"),
                QStringLiteral(".qix"), QStringLiteral(".sbn"), QStringLiteral(".sbx")
            };
            for (const QString& extension : sidecarExtensions)
                QFile::remove(outputBase + extension);
            appendLog(tr("  [WARN] 图层范围内无要素：%1").arg(layerInfo.tableName));
            continue;
        }

        QgsVectorFileWriter::SaveVectorOptions options;
        options.driverName = QStringLiteral("ESRI Shapefile");
        options.fileEncoding = QStringLiteral("UTF-8");
        options.layerName = outputLayerName;
        options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteFile;
        QString writerError;
        QString outputFile;
        const QgsVectorFileWriter::WriterError result = QgsVectorFileWriter::writeAsVectorFormatV3(
            &outputLayer, outputShp, outputLayer.transformContext(), options, &writerError, &outputFile);
        if (result != QgsVectorFileWriter::NoError)
        {
            if (errorMessage)
                *errorMessage = tr("导出图层 %1 失败：%2").arg(layerInfo.tableName, writerError);
            return false;
        }

        ++layersWithFeatures;
        appendLog(tr("  [OK] 成功裁剪并导出：%1 (%2 个要素)").arg(layerInfo.tableName).arg(featureCount));
    }

    if (loadedLayerCount == 0)
    {
        if (errorMessage)
            *errorMessage = tr("所选主题范围中的 %1 个空间图层均无法被 QGIS 加载。")
                                .arg(m_postgisSource.layers.size());
        return false;
    }
    if (layersWithFeatures == 0)
    {
        if (errorMessage)
            *errorMessage = tr("所选 PostGIS 图层在当前内图廓范围内均无要素。");
        return false;
    }
    appendLog(tr("PostGIS 裁剪完成：%1/%2 个可加载图层包含要素%3。")
                  .arg(layersWithFeatures)
                  .arg(loadedLayerCount)
                  .arg(skippedLayerCount > 0 ? tr("，已跳过 %1 个不可加载图层").arg(skippedLayerCount)
                                             : QString()));
    return true;
}

void CSE_PreprocessDialog::completeInternalSourceStep(bool success, const QString& errorMessage)
{
    if (m_cancelRequested)
        return;
    if (success)
    {
        appendLog(tr("✓ 步骤执行成功"));
        ++m_currentStep;
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
        return;
    }

    ++m_failedSteps;
    const int stepsPerPDF = mapStepsPerPdf();
    const int pdfIndex = m_currentStep / stepsPerPDF;
    const QString pdfName = (pdfIndex >= 0 && pdfIndex < ui.listWidget_PDFPaths->count())
        ? QFileInfo(ui.listWidget_PDFPaths->item(pdfIndex)->text()).baseName()
        : tr("未知 GeoPDF");
    const QString message = tr("✗ 当前步骤执行失败 [%1]：%2").arg(pdfName, errorMessage);
    appendLog(message);
    m_errorMessages.append(message);
    const int nextPdfFirstStep = (pdfIndex + 1) * stepsPerPDF;
    m_currentStep = nextPdfFirstStep < m_totalSteps ? nextPdfFirstStep : m_totalSteps;
    QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
}

// 符号库处理方法实现 (保持不变)
void CSE_PreprocessDialog::executeSymbolLaunchAI(int symbolIndex)
{
    QString scriptPath = getScriptFullPath("launch_ai2.py");

    if (!QFileInfo::exists(scriptPath))
    {
        appendLog(tr("错误: 找不到 launch_ai2.py 脚本文件"));
        m_currentStep++;
        processNextStep();
        return;
    }

    if (symbolIndex >= ui.listWidget_SymbolPDFPaths->count())
    {
        appendLog(tr("错误: 符号库PDF索引超出范围"));
        m_currentStep++;
        processNextStep();
        return;
    }

    QString symbolPdfPath = ui.listWidget_SymbolPDFPaths->item(symbolIndex)->text();

    // 在输出目录下创建"图例"子目录
    QString legendBaseDir = QDir(ui.lineEdit_SymbolOutputPath->text()).filePath("图例");
    QDir().mkpath(legendBaseDir);

    QStringList args;
    args << "--pdf-file" << symbolPdfPath;
    args << "--output-dir" << legendBaseDir;

    startPythonScript(scriptPath, args);
}

void CSE_PreprocessDialog::executeSymbolGeoJSON2Shp(int symbolIndex)
{
    QString scriptPath = getScriptFullPath("geojson2shp_symbols.py");

    if (!QFileInfo::exists(scriptPath))
    {
        appendLog(tr("错误: 找不到 geojson2shp_symbols.py 脚本文件"));
        m_currentStep++;
        processNextStep();
        return;
    }

    if (symbolIndex >= ui.listWidget_SymbolPDFPaths->count())
    {
        appendLog(tr("错误: 符号库PDF索引超出范围"));
        m_currentStep++;
        processNextStep();
        return;
    }

    QString symbolPdfPath = ui.listWidget_SymbolPDFPaths->item(symbolIndex)->text();
    QFileInfo pdfInfo(symbolPdfPath);
    QString baseName = pdfInfo.baseName();

    QString legendBaseDir = QDir(ui.lineEdit_SymbolOutputPath->text()).filePath("图例");
    QString geojsonFolder = QDir(legendBaseDir).filePath(baseName + "_geojson");
    QString outputFolder = QDir(legendBaseDir).filePath(baseName + "_shp");

    QStringList args;
    args << "--geojson-folder" << geojsonFolder;
    args << "--output-folder" << outputFolder;

    startPythonScript(scriptPath, args);
}

void CSE_PreprocessDialog::executeStandardLegendPdfPreprocess()
{
    QString scriptPath = getScriptFullPath("extract_standard_legend_pdf.py");

    if (!QFileInfo::exists(scriptPath))
    {
        appendLog(QString::fromUtf8(u8"错误: 找不到 extract_standard_legend_pdf.py 脚本文件"));
        m_currentStep++;
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
        return;
    }

    QString outputDir = QDir(ui.lineEdit_SymbolOutputPath->text()).filePath(QString::fromUtf8(u8"标准图例"));
    QDir().mkpath(outputDir);

    QStringList args;
    args << "-o" << outputDir;
    for (int i = 0; i < ui.listWidget_SymbolPDFPaths->count(); ++i)
    {
        args << ui.listWidget_SymbolPDFPaths->item(i)->text();
    }

    appendLog(QString::fromUtf8(u8"输出目录: %1").arg(outputDir));
    appendLog(QString::fromUtf8(u8"预期输出: standard_legend_table.csv、standard_legend_symbols.geojson、qgis_by_geometry/、qgis_by_symbol/"));

    QString pythonExecutable = getStandardLegendPythonPath();
    QStringList processArgs;
    processArgs << scriptPath;
    processArgs.append(args);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONDONTWRITEBYTECODE", "1");

    // 打包部署时可把 PyMuPDF/fitz 放在 preprocess_scripts/python_packages 下，
    // 这样不用依赖开发机上的 Anaconda 或用户额外安装 Python 包。
    QString bundledPackageDir = QFileInfo(scriptPath).absoluteDir().filePath("python_packages");
    if (QDir(bundledPackageDir).exists())
    {
        QString oldPythonPath = env.value("PYTHONPATH");
        env.insert("PYTHONPATH", oldPythonPath.isEmpty()
                                     ? bundledPackageDir
                                     : bundledPackageDir + QDir::listSeparator() + oldPythonPath);
    }
    m_pCurrentProcess->setProcessEnvironment(env);

    appendLog(QString::fromUtf8(u8"执行命令: %1 %2").arg(pythonExecutable, processArgs.join(" ")));
    m_pCurrentProcess->start(pythonExecutable, processArgs);
}

void CSE_PreprocessDialog::executeSymbolLegendMatch(int symbolIndex)
{
    {
        QString scriptPath = getScriptFullPath("legend_match.py");
        if (!QFileInfo::exists(scriptPath))
        {
            appendLog(QString::fromUtf8(u8"错误: 找不到 legend_match.py 脚本文件"));
            m_currentStep++;
            QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
            return;
        }

        if (symbolIndex >= ui.listWidget_SymbolPDFPaths->count())
        {
            appendLog(QString::fromUtf8(u8"错误: 图例SHP索引超出范围"));
            m_currentStep++;
            QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
            return;
        }

        QString selectedPath = ui.listWidget_SymbolPDFPaths->item(symbolIndex)->text();
        QFileInfo selectedInfo(selectedPath);
        QString shpFolder = selectedInfo.isDir() ? selectedInfo.absoluteFilePath() : selectedInfo.absolutePath();
        QString selectedShp = selectedInfo.isFile() ? selectedInfo.absoluteFilePath() : QString();
        QString baseName = QFileInfo(shpFolder).fileName();

        QDir shpDir(shpFolder);
        QString outputFolder = QDir(ui.lineEdit_SymbolOutputPath->text()).filePath(baseName + "_matched");
        QDir().mkpath(outputFolder);

        auto isGeneratedLegendOutput = [](const QString &fileName) {
            return fileName.contains("_matched", Qt::CaseInsensitive) ||
                   fileName.contains("_with_names", Qt::CaseInsensitive);
        };
        auto isLegendNamedShp = [&isGeneratedLegendOutput](const QString &fileName) {
            if (isGeneratedLegendOutput(fileName))
                return false;
            QString lower = fileName.toLower();
            return fileName.contains(QString::fromUtf8(u8"图例")) ||
                   fileName.startsWith(QString::fromUtf8(u8"要素")) ||
                   lower.contains("legend");
        };
        auto isLegendFeatureShp = [&isLegendNamedShp](const QString &fileName) {
            if (!isLegendNamedShp(fileName))
                return false;
            if (fileName.contains(QString::fromUtf8(u8"名称")))
                return false;
            QString lower = fileName.toLower();
            return lower.contains("polygon") ||
                   lower.contains("linestring") ||
                   lower.contains("point") ||
                   fileName == QString::fromUtf8(u8"要素.shp");
        };

        QString namesShp = shpDir.filePath(QString::fromUtf8(u8"要素名称.shp"));
        if (!QFileInfo::exists(namesShp))
        {
            QStringList nameCandidates = shpDir.entryList(QStringList() << QString::fromUtf8(u8"*名称*.shp"), QDir::Files);
            nameCandidates.erase(
                std::remove_if(nameCandidates.begin(), nameCandidates.end(), isGeneratedLegendOutput),
                nameCandidates.end());
            if (!nameCandidates.isEmpty())
            {
                namesShp = shpDir.filePath(nameCandidates.first());
            }
            else
            {
                QStringList mixedLegendNameCandidates = shpDir.entryList(
                    QStringList() << QString::fromUtf8(u8"*图例*Polygon*.shp")
                                  << QString::fromUtf8(u8"*legend*polygon*.shp"),
                    QDir::Files);
                if (mixedLegendNameCandidates.isEmpty())
                {
                    mixedLegendNameCandidates = shpDir.entryList(
                        QStringList() << QString::fromUtf8(u8"*图例*.shp")
                                      << QString::fromUtf8(u8"*legend*.shp"),
                        QDir::Files);
                }
                mixedLegendNameCandidates.erase(
                    std::remove_if(mixedLegendNameCandidates.begin(), mixedLegendNameCandidates.end(), isGeneratedLegendOutput),
                    mixedLegendNameCandidates.end());

                if (!mixedLegendNameCandidates.isEmpty())
                {
                    namesShp = shpDir.filePath(mixedLegendNameCandidates.first());
                    appendLog(QString::fromUtf8(u8"提示: 未找到独立名称图层，使用图例图层作为名称来源: %1").arg(namesShp));
                }
                else if (!selectedShp.isEmpty() && isLegendNamedShp(QFileInfo(selectedShp).fileName()))
                {
                    namesShp = selectedShp;
                    appendLog(QString::fromUtf8(u8"警告: 未找到要素名称.shp，临时使用所选SHP作为名称图层: %1").arg(namesShp));
                }
            }
        }

        if (!QFileInfo::exists(namesShp))
        {
            appendLog(QString::fromUtf8(u8"警告: 未找到名称SHP，跳过图例匹配: %1").arg(shpFolder));
            m_currentStep++;
            QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
            return;
        }

        QStringList featureFiles;
        QStringList standardFeatureNames;
        standardFeatureNames << QString::fromUtf8(u8"要素_Polygon.shp")
                             << QString::fromUtf8(u8"要素_LineString.shp")
                             << QString::fromUtf8(u8"要素.shp");

        for (const QString &fileName : standardFeatureNames)
        {
            QString featurePath = shpDir.filePath(fileName);
            if (QFileInfo::exists(featurePath))
            {
                featureFiles << featurePath;
            }
        }

        if (featureFiles.isEmpty())
        {
            QString namePath = QFileInfo(namesShp).absoluteFilePath();
            QStringList shpFiles = shpDir.entryList(QStringList() << "*.shp", QDir::Files);
            for (const QString &fileName : shpFiles)
            {
                if (!isLegendFeatureShp(fileName))
                    continue;
                QString featurePath = shpDir.filePath(fileName);
                if (QString::compare(QFileInfo(featurePath).absoluteFilePath(), namePath, Qt::CaseInsensitive) == 0)
                    continue;
                featureFiles << featurePath;
            }
        }

        if (featureFiles.isEmpty())
        {
            appendLog(QString::fromUtf8(u8"警告: 未找到需要匹配的符号SHP，跳过: %1").arg(shpFolder));
            m_currentStep++;
            QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
            return;
        }

        QString pythonExecutable = getQgisPythonPath();
        for (const QString &featShp : featureFiles)
        {
            QFileInfo fi(featShp);
            QString outputShp = QDir(outputFolder).filePath(fi.completeBaseName() + "_matched.shp");

            QStringList args;
            args << scriptPath;
            args << "--lines" << featShp;
            args << "--names" << namesShp;
            args << "--out" << outputShp;

            appendLog(QString::fromUtf8(u8"执行命令: %1 %2").arg(pythonExecutable, args.join(" ")));

            QProcess proc;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("PYTHONIOENCODING", "utf-8");
            proc.setProcessEnvironment(env);

            proc.start(pythonExecutable, args);
            bool finished = proc.waitForFinished(10 * 60 * 1000);
            QByteArray out = proc.readAllStandardOutput();
            QByteArray err = proc.readAllStandardError();

            QString outText = QString::fromUtf8(out);
            if (outText.isEmpty())
                outText = QString::fromLocal8Bit(out);
            if (!outText.trimmed().isEmpty())
                appendLog(outText.trimmed());

            QString errText = QString::fromUtf8(err);
            if (errText.isEmpty())
                errText = QString::fromLocal8Bit(err);
            if (!errText.trimmed().isEmpty())
                appendLog(QString::fromUtf8(u8"错误输出: ") + errText.trimmed());

            if (!finished)
            {
                appendLog(QString::fromUtf8(u8"错误: %1 未在超时内完成，跳过此要素").arg(featShp));
                m_failedSteps++;
                m_errorMessages.append(QString::fromUtf8(u8"要素处理超时: %1").arg(featShp));
                continue;
            }

            int exitCode = proc.exitCode();
            if (exitCode != 0)
            {
                appendLog(QString::fromUtf8(u8"错误: 脚本对要素 %1 返回非零退出码 %2，已记录并继续").arg(featShp).arg(exitCode));
                m_failedSteps++;
                m_errorMessages.append(QString::fromUtf8(u8"要素处理失败: %1 (exit %2)").arg(featShp).arg(exitCode));
                continue;
            }

            appendLog(QString::fromUtf8(u8"图例匹配完成: %1 -> %2").arg(featShp, outputShp));
        }

        m_currentStep++;
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
        return;
    }

    // 使用正确脚本名（注意拼写）
    QString scriptPath = getScriptFullPath("legend_match.py");

    if (!QFileInfo::exists(scriptPath))
    {
        appendLog(tr("错误: 找不到 legend_match.py 脚本文件"));
        m_currentStep++;
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
        return;
    }

    if (symbolIndex >= ui.listWidget_SymbolPDFPaths->count())
    {
        appendLog(tr("错误: 符号库PDF索引超出范围"));
        m_currentStep++;
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
        return;
    }

    QString symbolPdfPath = ui.listWidget_SymbolPDFPaths->item(symbolIndex)->text();
    QFileInfo pdfInfo(symbolPdfPath);
    QString baseName = pdfInfo.baseName();

    QString legendBaseDir = QDir(ui.lineEdit_SymbolOutputPath->text()).filePath("图例");
    QString shpFolder = QDir(legendBaseDir).filePath(baseName + "_shp");
    QString outputFolder = QDir(legendBaseDir).filePath(baseName + "_matched");
    QDir().mkpath(outputFolder);

    // 要素名称文件必须存在，否则跳过图例匹配
    QString namesShp = QDir(shpFolder).filePath("要素名称.shp");
    if (!QFileInfo::exists(namesShp))
    {
        appendLog(tr("警告: 未找到要素名称文件,跳过图例匹配"));
        m_currentStep++;
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
        return;
    }

    // 准备待处理的要素文件列表（可能包含三种形式）
    QStringList featureFiles;
    QString polygonShp = QDir(shpFolder).filePath("要素_Polygon.shp");
    QString lineShp = QDir(shpFolder).filePath("要素_LineString.shp");
    QString plainShp = QDir(shpFolder).filePath("要素.shp");

    if (QFileInfo::exists(polygonShp))
        featureFiles << polygonShp;
    if (QFileInfo::exists(lineShp))
        featureFiles << lineShp;
    if (QFileInfo::exists(plainShp))
        featureFiles << plainShp;

    if (featureFiles.isEmpty())
    {
        appendLog(tr("警告: 未找到任何要素文件 (要素_Polygon/要素_LineString/要素.shp)，跳过图例匹配"));
        m_currentStep++;
        QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
        return;
    }

    // 使用临时 QProcess 顺序执行每个要素文件的匹配，独立于 m_pCurrentProcess（避免影响主流程）
    QString pythonExecutable = getQgisPythonPath();
    for (const QString &featShp : featureFiles)
    {
        QFileInfo fi(featShp);
        QString inputBase = fi.completeBaseName(); // 保留原文件名，例如 要素_Polygon
        QString outputShp = QDir(outputFolder).filePath(inputBase + "_matched.shp");

        QStringList args;
        args << scriptPath;
        args << "--lines" << featShp;
        args << "--names" << namesShp;
        args << "--out" << outputShp;

        appendLog(tr("执行命令: %1 %2").arg(pythonExecutable, args.join(" ")));

        QProcess proc;
        // 强制 Python 使用 UTF-8 输出
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PYTHONIOENCODING", "utf-8");
        proc.setProcessEnvironment(env);

        proc.start(pythonExecutable, args);
        bool finished = proc.waitForFinished(10 * 60 * 1000); // 最长等待10分钟
        QByteArray out = proc.readAllStandardOutput();
        QByteArray err = proc.readAllStandardError();

        QString outText = QString::fromUtf8(out);
        if (outText.isEmpty())
            outText = QString::fromLocal8Bit(out);
        if (!outText.trimmed().isEmpty())
            appendLog(outText.trimmed());

        QString errText = QString::fromUtf8(err);
        if (errText.isEmpty())
            errText = QString::fromLocal8Bit(err);
        if (!errText.trimmed().isEmpty())
            appendLog(tr("错误输出: ") + errText.trimmed());

        if (!finished)
        {
            appendLog(tr("错误: %1 未在超时内完成，跳过此要素").arg(featShp));
            m_failedSteps++;
            m_errorMessages.append(tr("要素处理超时: %1").arg(featShp));
            continue;
        }

        int exitCode = proc.exitCode();
        if (exitCode != 0)
        {
            appendLog(tr("错误: 脚本对要素 %1 返回非零退出码 %2，已记录并继续").arg(featShp).arg(exitCode));
            m_failedSteps++;
            m_errorMessages.append(tr("要素处理失败: %1 (exit %2)").arg(featShp).arg(exitCode));
            continue;
        }

        appendLog(tr("图例匹配完成: %1 -> %2").arg(featShp, outputShp));
    }

    // 当前符号库的图例匹配全部尝试完毕，进入下一个步骤
    m_currentStep++;
    QMetaObject::invokeMethod(this, "processNextStep", Qt::QueuedConnection);
}

bool CSE_PreprocessDialog::validateInputParams()
{
    int currentTab = ui.tabWidget->currentIndex();

    if (currentTab == 0) // 地图预处理
    {
        if (ui.listWidget_PDFPaths->count() == 0)
        {
            QMessageBox::warning(this, tr("参数错误"), tr("请添加至少一个PDF文件"));
            return false;
        }

        if (ui.lineEdit_PDFOutputPath->text().isEmpty())
        {
            QMessageBox::warning(this, tr("参数错误"), tr("请设置输出路径"));
            return false;
        }

        if (currentSourceDataType() == SourceDataType::LocalGdb
            && !ui.lineEdit_SourceGDB->text().trimmed().isEmpty())
        {
            const QString gdbPath = ui.lineEdit_SourceGDB->text().trimmed();
            if (!QDir(gdbPath).exists() || !gdbPath.toLower().endsWith(".gdb"))
            {
                QMessageBox::warning(this, tr("参数错误"),
                                     tr("源GDB路径无效：%1\n请选择有效的 .gdb 目录").arg(gdbPath));
                return false;
            }
        }
        else if (currentSourceDataType() == SourceDataType::Postgis)
        {
            if (!hasSourceData())
            {
                QMessageBox::warning(this, tr("参数错误"),
                                     tr("请先连接 PostGIS 数据库并选择主题范围，或切换为本地 GDB。"));
                return false;
            }
            QString error;
            if (!loadPostgisSpatialLayers(&error))
            {
                QMessageBox::warning(this, tr("读取图层失败"),
                                     tr("无法读取所选主题范围的空间图层：%1").arg(error));
                return false;
            }
            // 每次开始处理前重新读取，确保主题关联关系和空间图层均为最新状态。
            m_postgisSource.layers = m_availablePostgisLayers;
            updateSourceDataUi();
        }
    }
    else // 符号库预处理
    {
        if (ui.listWidget_SymbolPDFPaths->count() == 0)
        {
            QMessageBox::warning(this, QString::fromUtf8(u8"参数错误"), QString::fromUtf8(u8"请添加至少一个符号库PDF文件"));
            return false;
        }

        if (ui.lineEdit_SymbolOutputPath->text().isEmpty())
        {
            QMessageBox::warning(this, QString::fromUtf8(u8"参数错误"), QString::fromUtf8(u8"请设置符号库输出路径"));
            return false;
        }
    }

    return true;
}

void CSE_PreprocessDialog::updateUIState(bool isRunning)
{
    ui.tabWidget->setEnabled(!isRunning);
    ui.pushButton_StartProcess->setEnabled(!isRunning);
    ui.pushButton_Cancel->setEnabled(isRunning);
    ui.pushButton_Close->setEnabled(!isRunning);
}

bool CSE_PreprocessDialog::isProcessingSymbols() const
{
    return m_isSymbolProcessing;
}

void CSE_PreprocessDialog::restoreState()
{
    QSettings settings;
    settings.beginGroup("PreprocessDialog");
    // 与“数据库连接”共享同一份数据库连接配置，预处理页无需重复录入。
    QSettings databaseSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));

    restoreGeometry(settings.value("geometry").toByteArray());

    PreprocessParams params;
    params.pdfOutputPath = settings.value("pdfOutputPath", "D:/Media/Desktop/preprocess_output").toString();
    params.sourceGdbPath = settings.value("sourceGdbPath", "").toString();
    params.sourceDataType = static_cast<SourceDataType>(
        settings.value("sourceDataType", static_cast<int>(SourceDataType::LocalGdb)).toInt());
    params.postgisSource.host = databaseSettings.value(
        QStringLiteral("db/host"), settings.value("postgis/host", "")).toString();
    params.postgisSource.port = databaseSettings.value(
        QStringLiteral("db/port"), settings.value("postgis/port", 5432)).toInt();
    params.postgisSource.database = databaseSettings.value(
        QStringLiteral("db/database"), settings.value("postgis/database", "")).toString();
    params.postgisSource.schema = databaseSettings.value(
        QStringLiteral("db/schema"), settings.value("postgis/schema", "public")).toString();
    params.postgisSource.user = databaseSettings.value(
        QStringLiteral("db/user"), settings.value("postgis/user", "")).toString();
    params.postgisSource.themeScope = static_cast<PostgisThemeScope>(
        settings.value("postgis/themeScope", static_cast<int>(PostgisThemeScope::Theme)).toInt());
    params.postgisSource.themeId = settings.value("postgis/themeId", 0).toLongLong();
    params.postgisSource.themeName = settings.value("postgis/themeName", "").toString();
    params.postgisSource.themeContentId = settings.value("postgis/themeContentId", 0).toLongLong();
    params.postgisSource.themeContentName = settings.value("postgis/themeContentName", "").toString();
    m_rememberPostgisPassword = databaseSettings.value(
        QStringLiteral("db/savePassword"), settings.value("postgis/rememberPassword", false)).toBool();
    if (m_rememberPostgisPassword)
        params.postgisSource.password = databaseSettings.value(
            QStringLiteral("db/password"), settings.value("postgis/password", "")).toString();
    params.symbolOutputPath = settings.value("symbolOutputPath", "D:/Media/Desktop/preprocess_output").toString();

    setPreprocessParams(params);

    settings.endGroup();
}

void CSE_PreprocessDialog::saveState()
{
    QSettings settings;
    settings.beginGroup("PreprocessDialog");
    QSettings databaseSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));

    settings.setValue("geometry", saveGeometry());

    PreprocessParams params = getPreprocessParams();
    settings.setValue("pdfOutputPath", params.pdfOutputPath);
    settings.setValue("sourceGdbPath", params.sourceGdbPath);
    settings.setValue("sourceDataType", static_cast<int>(params.sourceDataType));
    settings.setValue("postgis/host", params.postgisSource.host);
    settings.setValue("postgis/port", params.postgisSource.port);
    settings.setValue("postgis/database", params.postgisSource.database);
    settings.setValue("postgis/schema", params.postgisSource.schema);
    settings.setValue("postgis/user", params.postgisSource.user);
    settings.setValue("postgis/themeScope", static_cast<int>(params.postgisSource.themeScope));
    settings.setValue("postgis/themeId", params.postgisSource.themeId);
    settings.setValue("postgis/themeName", params.postgisSource.themeName);
    settings.setValue("postgis/themeContentId", params.postgisSource.themeContentId);
    settings.setValue("postgis/themeContentName", params.postgisSource.themeContentName);
    settings.setValue("postgis/rememberPassword", m_rememberPostgisPassword);
    settings.remove("postgis/layers");
    if (m_rememberPostgisPassword)
        settings.setValue("postgis/password", params.postgisSource.password);
    else
        settings.remove("postgis/password");

    // 同步到“数据库连接”的连接配置，使两个入口始终自动使用同一套信息。
    databaseSettings.setValue(QStringLiteral("db/host"), params.postgisSource.host);
    databaseSettings.setValue(QStringLiteral("db/port"), params.postgisSource.port);
    databaseSettings.setValue(QStringLiteral("db/database"), params.postgisSource.database);
    databaseSettings.setValue(QStringLiteral("db/schema"), params.postgisSource.schema);
    databaseSettings.setValue(QStringLiteral("db/user"), params.postgisSource.user);
    databaseSettings.setValue(QStringLiteral("db/savePassword"), m_rememberPostgisPassword);
    if (m_rememberPostgisPassword)
        databaseSettings.setValue(QStringLiteral("db/password"), params.postgisSource.password);
    else
        databaseSettings.remove(QStringLiteral("db/password"));
    settings.setValue("symbolOutputPath", params.symbolOutputPath);

    settings.endGroup();
}

// 包含MOC生成的文件
#include "moc_se_preprocess_dialog.cpp"
