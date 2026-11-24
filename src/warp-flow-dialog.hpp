/*
Warp
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <obs.h>

#include <QAbstractButton>
#include <QDialog>
#include <QIcon>
#include <QMap>
#include <QString>
#include <QStringList>

class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class WarpFlowSettings;

/* the "combo" kind only exists in the create dialog: what it makes is a replay
 * flow and a highlight flow, linked together */
#define WARP_FLOW_KIND_COMBO "combo"

QString warp_flow_text(const char *key);
QIcon warp_flow_icon(const char *kind);
QString warp_flow_kind_name(const char *kind);
/* a length in seconds the way the UI says it */
QString warp_length_label(int seconds);

/* Every key bound to anything, by the hotkey it is bound to, as the hotkey
 * settings would write it: "Shift+F9". */
QMap<obs_hotkey_id, QStringList> warp_bound_keys();
QString warp_key_text(const QMap<obs_hotkey_id, QStringList> &keys, obs_hotkey_id id);

/* the switch a flow is turned on and off with */
class WarpSwitch : public QAbstractButton {
public:
	explicit WarpSwitch(QWidget *parent);
	QSize sizeHint() const override { return QSize(36, 20); }

protected:
	void paintEvent(QPaintEvent *) override;
};

/* Picks what to make: the kinds down the left, and the settings the one that
 * is picked needs beside them. Everything it makes is made by the time it is
 * accepted. */
class WarpFlowCreateDialog : public QDialog {
public:
	explicit WarpFlowCreateDialog(QWidget *parent);

	/* the ids of the flows that were made, in the order they were made */
	QStringList createdIds() const { return created; }

	void accept() override;

private:
	QString kind() const;
	void refresh();
	bool buildFlow(const char *kind, const QString &name, bool highlight, const char *trigger, const char *order,
		       int maxClips, const QList<int> &lengths, const QStringList &links, QString &flowId);
	void linkFedBy(const QString &fedBy, const QString &flowId);

	QListWidget *kindList = nullptr;
	QLabel *kindTitle = nullptr;
	QLabel *kindDesc = nullptr;
	QLabel *chain = nullptr;
	QLineEdit *nameEdit = nullptr;
	WarpFlowSettings *settings = nullptr;
	QLabel *summary = nullptr;
	QPushButton *createButton = nullptr;

	QStringList created;
};

/* everything about a flow that is already there, kind aside; changes wait for
 * Apply */
class WarpFlowPropsDialog : public QDialog {
public:
	WarpFlowPropsDialog(QWidget *parent, const QString &flowId);

protected:
	void closeEvent(QCloseEvent *event) override;
	void reject() override;

private:
	void load();
	bool apply();
	void setDirty(bool value);
	void refreshStatus();

	QString flowId;
	QString kind;
	bool loading = false;
	bool dirty = false;

	QLineEdit *nameEdit = nullptr;
	QLabel *status = nullptr;
	QLabel *enabledText = nullptr;
	WarpSwitch *enabledSwitch = nullptr;
	WarpFlowSettings *settings = nullptr;
	QListWidget *sections = nullptr;
	QListWidget *linkList = nullptr;
	QLabel *fedBy = nullptr;
	QFormLayout *hotkeyForm = nullptr;
	QLabel *dirtyLabel = nullptr;
	QPushButton *revertButton = nullptr;
	QPushButton *applyButton = nullptr;
};
