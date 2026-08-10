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

#include <QDialog>
#include <QIcon>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

/* the "combo" kind only exists in the create dialog: what it makes is a replay
 * flow and a highlight flow, linked together */
#define WARP_FLOW_KIND_COMBO "combo"

QString warp_flow_text(const char *key);
QIcon warp_flow_icon(const char *kind);
QString warp_flow_kind_name(const char *kind);

/* Picks what to make, the way the Add Source dialog does: the kinds down the
 * left, what the one that is picked does on the right, and the settings it
 * needs underneath. Everything it makes is made by the time it is accepted. */
class WarpFlowCreateDialog : public QDialog {
public:
	explicit WarpFlowCreateDialog(QWidget *parent);

	/* the ids of the flows that were made, in the order they were made */
	QStringList createdIds() const { return created; }

	void accept() override;

private:
	void kindChanged();
	void targetChanged();
	bool buildFlow(const char *kind, const QString &name, QComboBox *targetCombo, QLineEdit *newTargetEdit,
		       const char *trigger, const char *order, int maxClips, const QStringList &links, QString &flowId);
	void linkFedBy(const QString &fedBy, const QString &flowId);

	QListWidget *kindList = nullptr;
	QLabel *kindTitle = nullptr;
	QLabel *kindDesc = nullptr;

	QFormLayout *form = nullptr;
	QLineEdit *nameEdit = nullptr;
	QComboBox *targetCombo = nullptr;
	QLineEdit *newTargetEdit = nullptr;
	QComboBox *triggerCombo = nullptr;
	QComboBox *orderCombo = nullptr;
	QCheckBox *limitCheck = nullptr;
	QSpinBox *limitSpin = nullptr;
	QComboBox *fedByCombo = nullptr;
	QComboBox *hlTargetCombo = nullptr;
	QLineEdit *hlNewTargetEdit = nullptr;
	QComboBox *hlOrderCombo = nullptr;
	QComboBox *playbackCombo = nullptr;
	QCheckBox *speedCheck = nullptr;
	QSpinBox *speedSpin = nullptr;

	/* the id of the source kind targetCombo is listing, so it is only filled
	 * again when the kind that is picked feeds a different one */
	QString targetSourceId;

	QStringList created;
};

/* everything about a flow that is already there, kind aside */
class WarpFlowPropsDialog : public QDialog {
public:
	WarpFlowPropsDialog(QWidget *parent, const QString &flowId);

	void accept() override;

private:
	void targetChanged();

	QString flowId;
	/* the kind cannot be changed here, so what it feeds and which fields
	 * mean anything are settled once, when the dialog is built */
	bool isInstant = false;
	QString targetSourceId;

	QFormLayout *form = nullptr;
	QLineEdit *nameEdit = nullptr;
	QComboBox *targetCombo = nullptr;
	QLineEdit *newTargetEdit = nullptr;
	QComboBox *triggerCombo = nullptr;
	QComboBox *orderCombo = nullptr;
	QCheckBox *limitCheck = nullptr;
	QSpinBox *limitSpin = nullptr;
	QComboBox *playbackCombo = nullptr;
	QCheckBox *speedCheck = nullptr;
	QSpinBox *speedSpin = nullptr;
	QCheckBox *enabledCheck = nullptr;
	QListWidget *linkList = nullptr;
};
