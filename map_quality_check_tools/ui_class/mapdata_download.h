#pragma once

#include <QDialog>
#include <QSqlDatabase>

#include "ui_mapdata_download.h"

/**
 * @brief 数据库连接对话框
 *
 * 建立应用级共享 PostGIS 连接，供地图数据预处理等功能复用。
 */
class mapdata_download : public QDialog
{
public:
	explicit mapdata_download(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~mapdata_download() override;

private:
	void setupConnectionPage();
	void setupConnections();
	void loadSettings();
	void saveSettings();
	void setStatus(const QString& message, const QString& color);
	bool openDatabase(QString* errorMessage = nullptr);
	void onConnect();

	Ui::mapdata_download ui;
	QSqlDatabase mDatabase;
	QString mConnectionName;
};
