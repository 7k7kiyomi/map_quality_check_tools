#include "mapdata_download.h"
#include "postgis_shared_connection.h"

#include <QApplication>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSqlError>
#include <QVBoxLayout>

mapdata_download::mapdata_download(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
	, mConnectionName(mapProductManagerSharedPostgisConnectionName())
{
	ui.setupUi(this);
	mDatabase = QSqlDatabase::contains(mConnectionName)
		? QSqlDatabase::database(mConnectionName)
		: QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), mConnectionName);

	setWindowTitle(QStringLiteral("数据库连接"));
	ui.mPasswordEdit->setEchoMode(QLineEdit::Password);
	setupConnectionPage();
	loadSettings();
	setupConnections();
}

mapdata_download::~mapdata_download()
{
	// 连接是应用级共享资源；数据预处理可在本窗口关闭后继续复用。
	mDatabase = QSqlDatabase();
}

void mapdata_download::setupConnectionPage()
{
	// .ui 文件仍由旧版本生成。运行时只复用其中的连接输入控件，
	// 其余下载相关控件一律不加入新的布局，避免再次暴露下载功能。
	delete layout();
	auto* pageLayout = new QVBoxLayout(this);
	pageLayout->setContentsMargins(12, 12, 12, 12);
	pageLayout->setSpacing(14);

	delete ui.groupBox->layout();
	ui.groupBox->setTitle(QStringLiteral("数据库连接参数"));
	ui.groupBox->setFixedHeight(292);
	auto* formLayout = new QFormLayout(ui.groupBox);
	formLayout->setContentsMargins(24, 18, 24, 18);
	formLayout->setHorizontalSpacing(12);
	formLayout->setVerticalSpacing(8);
	formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	ui.mHostEdit->setFixedHeight(32);
	ui.mPortSpin->setFixedHeight(32);
	ui.mDbNameEdit->setFixedHeight(32);
	ui.mSchemaCombo->setFixedHeight(32);
	ui.mUserEdit->setFixedHeight(32);
	ui.mPasswordEdit->setFixedHeight(32);
	formLayout->addRow(ui.hostLabel, ui.mHostEdit);
	formLayout->addRow(ui.portLabel, ui.mPortSpin);
	formLayout->addRow(ui.dbNameLabel, ui.mDbNameEdit);
	formLayout->addRow(ui.schemaLabel, ui.mSchemaCombo);
	formLayout->addRow(ui.userLabel, ui.mUserEdit);
	formLayout->addRow(ui.passwordLabel, ui.mPasswordEdit);
	pageLayout->addWidget(ui.groupBox);

	auto* statusGroup = new QGroupBox(QStringLiteral("连接状态"), this);
	statusGroup->setFixedHeight(130);
	auto* statusLayout = new QVBoxLayout(statusGroup);
	statusLayout->setContentsMargins(16, 16, 16, 16);
	ui.mStatusLabel->setParent(statusGroup);
	ui.mStatusLabel->setAlignment(Qt::AlignCenter);
	ui.mStatusLabel->setMinimumHeight(0);
	statusLayout->addWidget(ui.mStatusLabel, 1);
	pageLayout->addWidget(statusGroup);

	ui.mTestBtn->hide();
	ui.mConnectBtn->setText(QStringLiteral("连接"));
	ui.mConnectBtn->setToolTip(QStringLiteral("使用以上参数连接数据库"));
	ui.mConnectBtn->setMinimumWidth(88);
	auto* buttonLayout = new QHBoxLayout();
	ui.mSavePwdCheck->setParent(this);
	ui.mConnectBtn->setParent(this);
	buttonLayout->addWidget(ui.mSavePwdCheck);
	buttonLayout->addStretch();
	buttonLayout->addWidget(ui.mConnectBtn);
	pageLayout->addLayout(buttonLayout);

	// 所有这些控件均属于原下载页面，不应参与新页面的布局或交互。
	ui.searchGroup->hide();
	ui.mResultCountLabel->hide();
	ui.mSearchBtn->hide();
	ui.mResetBtn->hide();
	ui.mSelectAllBtn->hide();
	ui.mResultTable->hide();
	ui.mPrevPageBtn->hide();
	ui.mPageSpinBox->hide();
	ui.mTotalPageLabel->hide();
	ui.mNextPageBtn->hide();
	ui.outputPathGroup->hide();

	// 数据库连接页采用固定尺寸，避免在插件宿主中被压缩后表单行重叠。
	setFixedSize(630, 510);
}

void mapdata_download::setupConnections()
{
	connect(ui.mConnectBtn, &QPushButton::clicked, this, &mapdata_download::onConnect);
}

void mapdata_download::loadSettings()
{
	QSettings settings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
	ui.mHostEdit->setText(settings.value(QStringLiteral("db/host"), QStringLiteral("localhost")).toString());
	ui.mPortSpin->setValue(settings.value(QStringLiteral("db/port"), 5432).toInt());
	ui.mDbNameEdit->setText(settings.value(QStringLiteral("db/database"), QStringLiteral("ltzk_platform")).toString());
	ui.mSchemaCombo->setCurrentText(settings.value(QStringLiteral("db/schema"), QStringLiteral("public")).toString());
	ui.mUserEdit->setText(settings.value(QStringLiteral("db/user"), QStringLiteral("YOUR_PASSWORD")).toString());
	ui.mSavePwdCheck->setChecked(settings.value(QStringLiteral("db/savePassword"), false).toBool());
	if (ui.mSavePwdCheck->isChecked())
		ui.mPasswordEdit->setText(settings.value(QStringLiteral("db/password")).toString());
	setStatus(QStringLiteral("未连接"), QStringLiteral("#444444"));
}

void mapdata_download::saveSettings()
{
	QSettings settings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
	settings.setValue(QStringLiteral("db/host"), ui.mHostEdit->text().trimmed());
	settings.setValue(QStringLiteral("db/port"), ui.mPortSpin->value());
	settings.setValue(QStringLiteral("db/database"), ui.mDbNameEdit->text().trimmed());
	settings.setValue(QStringLiteral("db/schema"), ui.mSchemaCombo->currentText().trimmed());
	settings.setValue(QStringLiteral("db/user"), ui.mUserEdit->text().trimmed());
	settings.setValue(QStringLiteral("db/savePassword"), ui.mSavePwdCheck->isChecked());
	if (ui.mSavePwdCheck->isChecked())
		settings.setValue(QStringLiteral("db/password"), ui.mPasswordEdit->text());
	else
		settings.remove(QStringLiteral("db/password"));
}

void mapdata_download::setStatus(const QString& message, const QString& color)
{
	ui.mStatusLabel->setText(message);
	ui.mStatusLabel->setStyleSheet(
		QStringLiteral("QLabel { color: %1; font-weight: bold; padding: 4px; }").arg(color));
}

bool mapdata_download::openDatabase(QString* errorMessage)
{
	if (mDatabase.isOpen())
		mDatabase.close();
	mDatabase.setHostName(ui.mHostEdit->text().trimmed());
	mDatabase.setPort(ui.mPortSpin->value());
	mDatabase.setDatabaseName(ui.mDbNameEdit->text().trimmed());
	mDatabase.setUserName(ui.mUserEdit->text().trimmed());
	mDatabase.setPassword(ui.mPasswordEdit->text());
	if (mDatabase.open())
		return true;
	if (errorMessage)
		*errorMessage = mDatabase.lastError().text();
	return false;
}

void mapdata_download::onConnect()
{
	ui.mConnectBtn->setEnabled(false);
	setStatus(QStringLiteral("正在连接数据库…"), QStringLiteral("#b36b00"));
	QApplication::processEvents();

	QString error;
	if (openDatabase(&error))
	{
		saveSettings();
		setStatus(QStringLiteral("已连接"), QStringLiteral("#187a36"));
	}
	else
	{
		setStatus(QStringLiteral("连接失败：") + error, QStringLiteral("#b42318"));
	}
	ui.mConnectBtn->setEnabled(true);
}
