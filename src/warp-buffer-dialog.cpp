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

#include <cstring>

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "warp-buffer-dialog.hpp"
#include "warp-buffer.h"
#include "warp-flow-dialog.hpp"

namespace {

/* the columns of the angle list */
enum {
	WARP_ANGLE_COL_NAME,
	WARP_ANGLE_COL_FEED,
	WARP_ANGLE_COL_COUNT,
};

/* what an angle row carries besides its two columns */
constexpr int WARP_ANGLE_ROLE_ID = Qt::UserRole;
constexpr int WARP_ANGLE_ROLE_UUID = Qt::UserRole + 1;
constexpr int WARP_ANGLE_ROLE_SOURCE = Qt::UserRole + 2;

/* the lengths a new buffer is set up with: the two an operator reaches for
 * most, and enough of a hint at what the list is for */
constexpr int WARP_BUFFER_DEFAULT_LENGTHS[] = {10, 30};

bool warp_buffer_enum_source(void *param, obs_source_t *source)
{
	auto *combo = static_cast<QComboBox *>(param);

	/* anything with a picture can be held on an angle of its own, scenes
	 * included: a buffer on a scene is that scene whatever is on air */
	if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
		const char *name = obs_source_get_name(source);
		const char *uuid = obs_source_get_uuid(source);

		if (name && *name)
			combo->addItem(QString::fromUtf8(name), QString::fromUtf8(uuid ? uuid : ""));
	}

	return true;
}

/* Every feed an angle can hold: the programme first, since it is what a buffer
 * holds unless it is told otherwise, then every source with a picture. */
void warp_fill_angle_source_combo(QComboBox *combo)
{
	combo->clear();
	combo->addItem(warp_flow_text("Warp.Buffer.Angle.Program"), QString());

	obs_enum_sources(warp_buffer_enum_source, combo);
	obs_enum_scenes(warp_buffer_enum_source, combo);
}

QTreeWidgetItem *warp_make_angle_row(QTreeWidget *tree, const QString &name, const QString &uuid,
				     const QString &source_name, const QString &angle_id)
{
	auto *item = new QTreeWidgetItem(tree);

	item->setText(WARP_ANGLE_COL_NAME, name);
	item->setText(WARP_ANGLE_COL_FEED, uuid.isEmpty() ? warp_flow_text("Warp.Buffer.Angle.Program") : source_name);
	item->setData(WARP_ANGLE_COL_NAME, WARP_ANGLE_ROLE_ID, angle_id);
	item->setData(WARP_ANGLE_COL_NAME, WARP_ANGLE_ROLE_UUID, uuid);
	item->setData(WARP_ANGLE_COL_NAME, WARP_ANGLE_ROLE_SOURCE, source_name);

	return item;
}

} // namespace

WarpBufferDialog::WarpBufferDialog(QWidget *parent, const QString &bufferId) : QDialog(parent), id(bufferId)
{
	obs_data_t *config = id.isEmpty() ? nullptr : warp_buffer_get(id.toUtf8().constData());

	setWindowTitle(warp_flow_text(config ? "Warp.Buffer.Props.Title" : "Warp.Buffer.Add.Title"));
	resize(600, 620);

	auto *intro = new QLabel(warp_flow_text("Warp.Buffer.Intro"), this);

	intro->setWordWrap(true);
	intro->setTextFormat(Qt::PlainText);

	nameEdit = new QLineEdit(this);
	nameEdit->setText(QString::fromUtf8(config ? obs_data_get_string(config, WARP_BUFFER_NAME) : ""));

	auto *nameRow = new QHBoxLayout();

	nameRow->addWidget(new QLabel(warp_flow_text("Warp.Buffer.Name"), this));
	nameRow->addWidget(nameEdit, 1);

	/* ------------------------------------------------------------------ */
	/* the angles */

	auto *angleBox = new QGroupBox(warp_flow_text("Warp.Buffer.Angles"), this);
	auto *angleDesc = new QLabel(warp_flow_text("Warp.Buffer.Angles.Desc"), angleBox);

	angleDesc->setWordWrap(true);
	angleDesc->setTextFormat(Qt::PlainText);

	angleTree = new QTreeWidget(angleBox);
	angleTree->setColumnCount(WARP_ANGLE_COL_COUNT);
	angleTree->setRootIsDecorated(false);
	angleTree->setAlternatingRowColors(true);
	angleTree->setSelectionMode(QAbstractItemView::SingleSelection);
	angleTree->setHeaderLabels(
		{warp_flow_text("Warp.Buffer.Angle.Name"), warp_flow_text("Warp.Buffer.Angle.Feed")});
	angleTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
	angleTree->header()->setStretchLastSection(true);

	angleSource = new QComboBox(angleBox);
	warp_fill_angle_source_combo(angleSource);

	angleName = new QLineEdit(angleBox);
	angleName->setPlaceholderText(warp_flow_text("Warp.Buffer.Angle.Name.Placeholder"));

	angleAdd = new QPushButton(warp_flow_text("Warp.Buffer.Angle.Add"), angleBox);
	angleRemove = new QPushButton(warp_flow_text("Warp.Buffer.Angle.Remove"), angleBox);
	angleUp = new QPushButton(warp_flow_text("Warp.Buffer.Angle.Up"), angleBox);
	angleDown = new QPushButton(warp_flow_text("Warp.Buffer.Angle.Down"), angleBox);

	angleUp->setToolTip(warp_flow_text("Warp.Buffer.Angle.Order.Desc"));
	angleDown->setToolTip(warp_flow_text("Warp.Buffer.Angle.Order.Desc"));

	auto *angleAddRow = new QHBoxLayout();

	angleAddRow->addWidget(angleSource, 2);
	angleAddRow->addWidget(angleName, 1);
	angleAddRow->addWidget(angleAdd);

	auto *angleButtons = new QHBoxLayout();

	angleButtons->addWidget(angleRemove);
	angleButtons->addStretch(1);
	angleButtons->addWidget(angleUp);
	angleButtons->addWidget(angleDown);

	auto *angleLayout = new QVBoxLayout(angleBox);

	angleLayout->addWidget(angleDesc);
	angleLayout->addWidget(angleTree, 1);
	angleLayout->addLayout(angleAddRow);
	angleLayout->addLayout(angleButtons);

	/* ------------------------------------------------------------------ */
	/* the lengths */

	auto *lengthBox = new QGroupBox(warp_flow_text("Warp.Buffer.Lengths"), this);
	auto *lengthDesc = new QLabel(warp_flow_text("Warp.Buffer.Lengths.Desc"), lengthBox);

	lengthDesc->setWordWrap(true);
	lengthDesc->setTextFormat(Qt::PlainText);

	lengthList = new QListWidget(lengthBox);
	lengthList->setSelectionMode(QAbstractItemView::SingleSelection);

	lengthSpin = new QSpinBox(lengthBox);
	lengthSpin->setRange(WARP_BUFFER_LENGTH_MIN, WARP_BUFFER_LENGTH_MAX);
	lengthSpin->setValue(10);
	lengthSpin->setSuffix(warp_flow_text("Warp.Buffer.Seconds.Suffix"));

	lengthAdd = new QPushButton(warp_flow_text("Warp.Buffer.Length.Add"), lengthBox);
	lengthRemove = new QPushButton(warp_flow_text("Warp.Buffer.Length.Remove"), lengthBox);

	auto *lengthRow = new QHBoxLayout();

	lengthRow->addWidget(lengthSpin);
	lengthRow->addWidget(lengthAdd);
	lengthRow->addStretch(1);
	lengthRow->addWidget(lengthRemove);

	auto *lengthLayout = new QVBoxLayout(lengthBox);

	lengthLayout->addWidget(lengthDesc);
	lengthLayout->addWidget(lengthList, 1);
	lengthLayout->addLayout(lengthRow);

	/* ------------------------------------------------------------------ */

	followCheck = new QCheckBox(warp_flow_text("Warp.Buffer.FollowObs"), this);
	followCheck->setToolTip(warp_flow_text("Warp.Buffer.FollowObs.Desc"));
	followCheck->setChecked(config ? obs_data_get_bool(config, WARP_BUFFER_FOLLOW_OBS) : false);

	sizeSpin = new QSpinBox(this);
	sizeSpin->setRange(0, 32768);
	sizeSpin->setSuffix(warp_flow_text("Warp.Buffer.Megabytes.Suffix"));
	sizeSpin->setSpecialValueText(warp_flow_text("Warp.Buffer.MaxSize.None"));
	sizeSpin->setValue(config ? (int)obs_data_get_int(config, WARP_BUFFER_MAX_SIZE_MB) : 512);
	sizeSpin->setToolTip(warp_flow_text("Warp.Buffer.MaxSize.Desc"));

	auto *sizeRow = new QHBoxLayout();

	sizeRow->addWidget(new QLabel(warp_flow_text("Warp.Buffer.MaxSize"), this));
	sizeRow->addWidget(sizeSpin);
	sizeRow->addStretch(1);

	estimate = new QLabel(this);
	estimate->setWordWrap(true);
	estimate->setTextFormat(Qt::PlainText);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);

	layout->addWidget(intro);
	layout->addLayout(nameRow);
	layout->addWidget(angleBox, 2);
	layout->addWidget(lengthBox, 1);
	layout->addWidget(followCheck);
	layout->addLayout(sizeRow);
	layout->addWidget(estimate);
	layout->addWidget(buttons);

	/* ------------------------------------------------------------------ */
	/* what is already there */

	obs_data_array_t *angles = config ? obs_data_get_array(config, WARP_BUFFER_ANGLES) : nullptr;
	const size_t angle_count = angles ? obs_data_array_count(angles) : 0;

	for (size_t i = 0; i < angle_count; i++) {
		obs_data_t *angle = obs_data_array_item(angles, i);
		const char *feed = obs_data_get_string(angle, WARP_BUFFER_ANGLE_FEED);
		const bool program = !feed || !*feed || strcmp(feed, WARP_BUFFER_FEED_SOURCE) != 0;

		warp_make_angle_row(
			angleTree, QString::fromUtf8(obs_data_get_string(angle, WARP_BUFFER_ANGLE_NAME)),
			program ? QString()
				: QString::fromUtf8(obs_data_get_string(angle, WARP_BUFFER_ANGLE_SOURCE_UUID)),
			QString::fromUtf8(obs_data_get_string(angle, WARP_BUFFER_ANGLE_SOURCE_NAME)),
			QString::fromUtf8(obs_data_get_string(angle, WARP_BUFFER_ANGLE_ID)));

		obs_data_release(angle);
	}

	obs_data_array_release(angles);

	obs_data_array_t *lengths = config ? obs_data_get_array(config, WARP_BUFFER_LENGTHS) : nullptr;
	const size_t length_count = lengths ? obs_data_array_count(lengths) : 0;

	for (size_t i = 0; i < length_count; i++) {
		obs_data_t *length = obs_data_array_item(lengths, i);
		const int seconds = (int)obs_data_get_int(length, WARP_BUFFER_LENGTH_SECONDS);

		if (seconds >= WARP_BUFFER_LENGTH_MIN && seconds <= WARP_BUFFER_LENGTH_MAX) {
			auto *item =
				new QListWidgetItem(warp_flow_text("Warp.Buffer.Length.Item").arg(seconds), lengthList);

			item->setData(Qt::UserRole, seconds);
		}

		obs_data_release(length);
	}

	obs_data_array_release(lengths);

	/* A new buffer arrives holding the programme at two lengths, which is
	 * what OBS's own replay buffer does with one length - so it is worth
	 * something the moment it is started, and what the lists are for is
	 * plain from what is in them. */
	if (!config) {
		warp_make_angle_row(angleTree, warp_flow_text("Warp.Buffer.Angle.Program"), QString(), QString(),
				    QString());

		for (int seconds : WARP_BUFFER_DEFAULT_LENGTHS) {
			auto *item =
				new QListWidgetItem(warp_flow_text("Warp.Buffer.Length.Item").arg(seconds), lengthList);

			item->setData(Qt::UserRole, seconds);
		}
	}

	connect(angleAdd, &QPushButton::clicked, this, [this]() { addAngle(); });
	connect(angleRemove, &QPushButton::clicked, this, [this]() { removeAngle(); });
	connect(angleUp, &QPushButton::clicked, this, [this]() { moveAngle(-1); });
	connect(angleDown, &QPushButton::clicked, this, [this]() { moveAngle(1); });
	connect(angleTree, &QTreeWidget::itemSelectionChanged, this, [this]() { angleSelectionChanged(); });

	connect(lengthAdd, &QPushButton::clicked, this, [this]() { addLength(); });
	connect(lengthRemove, &QPushButton::clicked, this, [this]() { removeLength(); });
	connect(lengthList, &QListWidget::itemSelectionChanged, this, [this]() { lengthSelectionChanged(); });

	angleSelectionChanged();
	lengthSelectionChanged();
	refreshEstimate();

	obs_data_release(config);
}

void WarpBufferDialog::addAngle()
{
	if (angleTree->topLevelItemCount() >= WARP_BUFFER_MAX_ANGLES) {
		QMessageBox::warning(this, windowTitle(),
				     warp_flow_text("Warp.Buffer.Error.TooManyAngles").arg(WARP_BUFFER_MAX_ANGLES));
		return;
	}

	const QString uuid = angleSource->currentData().toString();
	const QString source_name = uuid.isEmpty() ? QString() : angleSource->currentText();
	QString name = angleName->text().trimmed();

	/* left unnamed, an angle is called after whatever it holds, which is
	 * the name an operator would have typed anyway */
	if (name.isEmpty())
		name = uuid.isEmpty() ? warp_flow_text("Warp.Buffer.Angle.Program") : source_name;

	warp_make_angle_row(angleTree, name, uuid, source_name, QString());

	angleName->clear();
	angleSelectionChanged();
	refreshEstimate();
}

void WarpBufferDialog::removeAngle()
{
	delete angleTree->currentItem();

	angleSelectionChanged();
	refreshEstimate();
}

void WarpBufferDialog::moveAngle(int delta)
{
	QTreeWidgetItem *item = angleTree->currentItem();

	if (!item)
		return;

	const int from = angleTree->indexOfTopLevelItem(item);
	const int to = from + delta;

	if (to < 0 || to >= angleTree->topLevelItemCount())
		return;

	angleTree->takeTopLevelItem(from);
	angleTree->insertTopLevelItem(to, item);
	angleTree->setCurrentItem(item);

	angleSelectionChanged();
}

void WarpBufferDialog::addLength()
{
	const int seconds = lengthSpin->value();

	if (lengthList->count() >= WARP_BUFFER_MAX_LENGTHS) {
		QMessageBox::warning(this, windowTitle(),
				     warp_flow_text("Warp.Buffer.Error.TooManyLengths").arg(WARP_BUFFER_MAX_LENGTHS));
		return;
	}

	for (int i = 0; i < lengthList->count(); i++) {
		if (lengthList->item(i)->data(Qt::UserRole).toInt() == seconds) {
			lengthList->setCurrentRow(i);
			return;
		}
	}

	auto *item = new QListWidgetItem(warp_flow_text("Warp.Buffer.Length.Item").arg(seconds), lengthList);

	item->setData(Qt::UserRole, seconds);

	lengthSelectionChanged();
	refreshEstimate();
}

void WarpBufferDialog::removeLength()
{
	delete lengthList->currentItem();

	lengthSelectionChanged();
	refreshEstimate();
}

void WarpBufferDialog::angleSelectionChanged()
{
	QTreeWidgetItem *item = angleTree->currentItem();
	const int at = item ? angleTree->indexOfTopLevelItem(item) : -1;

	angleRemove->setEnabled(at >= 0);
	angleUp->setEnabled(at > 0);
	angleDown->setEnabled(at >= 0 && at < angleTree->topLevelItemCount() - 1);
}

void WarpBufferDialog::lengthSelectionChanged()
{
	lengthRemove->setEnabled(lengthList->currentItem() != nullptr);
}

void WarpBufferDialog::refreshEstimate()
{
	int seconds = 0;

	for (int i = 0; i < lengthList->count(); i++)
		seconds += lengthList->item(i)->data(Qt::UserRole).toInt();

	const int angles = angleTree->topLevelItemCount();

	if (!seconds || !angles) {
		estimate->setText(warp_flow_text("Warp.Buffer.Estimate.Empty"));
		return;
	}

	/* the same sum warp_buffer_memory_estimate() does, over what is in the
	 * dialog rather than over what is saved, so the cost of another angle
	 * is there before the buffer is */
	const int megabytes = warp_buffer_memory_of(angles, seconds);

	estimate->setText(warp_flow_text("Warp.Buffer.Estimate").arg(angles).arg(angles * seconds).arg(megabytes));
}

void WarpBufferDialog::accept()
{
	const QString name = nameEdit->text().trimmed();

	if (name.isEmpty()) {
		QMessageBox::warning(this, windowTitle(), warp_flow_text("Warp.Buffer.Error.NameEmpty"));
		nameEdit->setFocus();
		return;
	}

	if (!angleTree->topLevelItemCount()) {
		QMessageBox::warning(this, windowTitle(), warp_flow_text("Warp.Buffer.Error.NoAngles"));
		return;
	}

	if (!lengthList->count()) {
		QMessageBox::warning(this, windowTitle(), warp_flow_text("Warp.Buffer.Error.NoLengths"));
		return;
	}

	/* two buffers with one name would leave the websocket and the flow
	 * dialog guessing which was meant */
	obs_data_t *clash = warp_buffer_get_by_name(name.toUtf8().constData());

	if (clash) {
		const QString clash_id = QString::fromUtf8(obs_data_get_string(clash, WARP_BUFFER_ID));

		obs_data_release(clash);

		if (clash_id != id) {
			QMessageBox::warning(this, windowTitle(),
					     warp_flow_text("Warp.Buffer.Error.NameTaken").arg(name));
			nameEdit->setFocus();
			return;
		}
	}

	obs_data_t *config = obs_data_create();
	obs_data_array_t *angles = obs_data_array_create();
	obs_data_array_t *lengths = obs_data_array_create();

	obs_data_set_string(config, WARP_BUFFER_NAME, name.toUtf8().constData());

	for (int i = 0; i < angleTree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = angleTree->topLevelItem(i);
		const QString uuid = item->data(WARP_ANGLE_COL_NAME, WARP_ANGLE_ROLE_UUID).toString();
		const QString source = item->data(WARP_ANGLE_COL_NAME, WARP_ANGLE_ROLE_SOURCE).toString();
		const QString angle_id = item->data(WARP_ANGLE_COL_NAME, WARP_ANGLE_ROLE_ID).toString();
		obs_data_t *angle = obs_data_create();

		obs_data_set_string(angle, WARP_BUFFER_ANGLE_ID, angle_id.toUtf8().constData());
		obs_data_set_string(angle, WARP_BUFFER_ANGLE_NAME,
				    item->text(WARP_ANGLE_COL_NAME).toUtf8().constData());
		obs_data_set_string(angle, WARP_BUFFER_ANGLE_FEED,
				    uuid.isEmpty() ? WARP_BUFFER_FEED_PROGRAM : WARP_BUFFER_FEED_SOURCE);
		obs_data_set_string(angle, WARP_BUFFER_ANGLE_SOURCE_UUID, uuid.toUtf8().constData());
		obs_data_set_string(angle, WARP_BUFFER_ANGLE_SOURCE_NAME, source.toUtf8().constData());

		obs_data_array_push_back(angles, angle);
		obs_data_release(angle);
	}

	for (int i = 0; i < lengthList->count(); i++) {
		obs_data_t *length = obs_data_create();

		obs_data_set_int(length, WARP_BUFFER_LENGTH_SECONDS, lengthList->item(i)->data(Qt::UserRole).toInt());
		obs_data_array_push_back(lengths, length);
		obs_data_release(length);
	}

	obs_data_set_array(config, WARP_BUFFER_ANGLES, angles);
	obs_data_set_array(config, WARP_BUFFER_LENGTHS, lengths);
	obs_data_set_bool(config, WARP_BUFFER_FOLLOW_OBS, followCheck->isChecked());
	obs_data_set_int(config, WARP_BUFFER_MAX_SIZE_MB, sizeSpin->value());

	if (id.isEmpty()) {
		char *made = warp_buffer_add(config);

		id = QString::fromUtf8(made ? made : "");
		bfree(made);
	} else {
		warp_buffer_update(id.toUtf8().constData(), config);
	}

	obs_data_array_release(lengths);
	obs_data_array_release(angles);
	obs_data_release(config);

	QDialog::accept();
}
