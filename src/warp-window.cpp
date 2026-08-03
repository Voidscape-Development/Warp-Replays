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
#include <obs-frontend-api.h>

#include <QApplication>
#include <QDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "warp-flow-dialog.hpp"
#include "warp-flow.h"

namespace {

enum WarpFlowColumn {
	WARP_COL_NAME,
	WARP_COL_KIND,
	WARP_COL_TARGET,
	WARP_COL_CLIPS,
	WARP_COL_TRIGGER,
	WARP_COL_ORDER,
	WARP_COL_LINKS,
	WARP_COL_COUNT,
};

/* The Warp window: the flows of the scene collection, and what is being made
 * of them. Everything that changes one is done from here, so the list is
 * rebuilt after every one of them rather than watched for. */
class WarpWindow : public QDialog {
public:
	explicit WarpWindow(QWidget *parent);
	~WarpWindow() override;

	/* the flow list from the top */
	void refresh();
	/* what does not need the list rebuilt: the replay buffer, and how many
	 * clips each flow's playlist is holding */
	void refreshStatus();

private:
	void addFlow();
	void editFlow();
	void removeFlow();
	void selectionChanged();
	QString selectedId() const;

	QTreeWidget *tree = nullptr;
	QPushButton *propsButton = nullptr;
	QPushButton *removeButton = nullptr;
	QLabel *status = nullptr;
	QTimer *timer = nullptr;
};

void warp_window_frontend_event(enum obs_frontend_event event, void *data)
{
	auto *window = static_cast<WarpWindow *>(data);

	switch (event) {
	/* the flows belong to the scene collection, so they are not the ones
	 * that were on screen a moment ago */
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		QMetaObject::invokeMethod(window, [window]() { window->refresh(); }, Qt::QueuedConnection);
		break;
	default:
		break;
	}
}

QString warp_trigger_name(const char *trigger)
{
	if (trigger && strcmp(trigger, WARP_FLOW_TRIGGER_LISTEN) == 0)
		return warp_flow_text("Warp.Flow.Trigger.Listen");

	return warp_flow_text("Warp.Flow.Trigger.Hotkey");
}

QString warp_order_name(const char *order)
{
	if (order && strcmp(order, WARP_FLOW_ORDER_NEWEST_FIRST) == 0)
		return warp_flow_text("Warp.Flow.Order.NewestFirst");

	return warp_flow_text("Warp.Flow.Order.OldestFirst");
}

WarpWindow::WarpWindow(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(warp_flow_text("Warp.Window.Title"));
	resize(940, 460);

	auto *intro = new QLabel(warp_flow_text("Warp.Window.Intro"), this);

	intro->setWordWrap(true);
	intro->setTextFormat(Qt::PlainText);

	tree = new QTreeWidget(this);
	tree->setColumnCount(WARP_COL_COUNT);
	tree->setRootIsDecorated(false);
	tree->setAlternatingRowColors(true);
	tree->setSelectionMode(QAbstractItemView::SingleSelection);
	tree->setUniformRowHeights(true);
	tree->setIconSize(QSize(20, 20));
	tree->setHeaderLabels({warp_flow_text("Warp.Window.Column.Flow"), warp_flow_text("Warp.Window.Column.Kind"),
			       warp_flow_text("Warp.Window.Column.Target"), warp_flow_text("Warp.Window.Column.Clips"),
			       warp_flow_text("Warp.Window.Column.Trigger"), warp_flow_text("Warp.Window.Column.Order"),
			       warp_flow_text("Warp.Window.Column.Links")});
	tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
	tree->header()->setStretchLastSection(true);

	auto *addButton = new QPushButton(warp_flow_text("Warp.Window.Add"), this);

	propsButton = new QPushButton(warp_flow_text("Warp.Window.Props"), this);
	removeButton = new QPushButton(warp_flow_text("Warp.Window.Remove"), this);

	auto *closeButton = new QPushButton(warp_flow_text("Warp.Window.Close"), this);

	closeButton->setDefault(true);

	status = new QLabel(this);
	status->setTextFormat(Qt::PlainText);

	auto *buttons = new QHBoxLayout();

	buttons->addWidget(addButton);
	buttons->addWidget(propsButton);
	buttons->addWidget(removeButton);
	buttons->addStretch(1);
	buttons->addWidget(closeButton);

	auto *layout = new QVBoxLayout(this);

	layout->addWidget(intro);
	layout->addWidget(tree, 1);
	layout->addWidget(status);
	layout->addLayout(buttons);

	connect(addButton, &QPushButton::clicked, this, [this]() { addFlow(); });
	connect(propsButton, &QPushButton::clicked, this, [this]() { editFlow(); });
	connect(removeButton, &QPushButton::clicked, this, [this]() { removeFlow(); });
	connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
	connect(tree, &QTreeWidget::itemSelectionChanged, this, [this]() { selectionChanged(); });
	connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *, int) { editFlow(); });

	timer = new QTimer(this);
	timer->setInterval(1000);
	connect(timer, &QTimer::timeout, this, [this]() { refreshStatus(); });
	timer->start();

	obs_frontend_add_event_callback(warp_window_frontend_event, this);

	refresh();
}

WarpWindow::~WarpWindow()
{
	obs_frontend_remove_event_callback(warp_window_frontend_event, this);
}

QString WarpWindow::selectedId() const
{
	QTreeWidgetItem *item = tree->currentItem();

	return item ? item->data(WARP_COL_NAME, Qt::UserRole).toString() : QString();
}

void WarpWindow::selectionChanged()
{
	const bool has = !selectedId().isEmpty();

	propsButton->setEnabled(has);
	removeButton->setEnabled(has);
}

void WarpWindow::refresh()
{
	const QString selected = selectedId();

	tree->clear();

	obs_data_array_t *flows = warp_flow_list();
	const size_t count = obs_data_array_count(flows);

	/* the name of every flow, to say what a link points at */
	QMap<QString, QString> names;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *flow = obs_data_array_item(flows, i);

		names.insert(QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID)),
			     QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME)));
		obs_data_release(flow);
	}

	for (size_t i = 0; i < count; i++) {
		obs_data_t *flow = obs_data_array_item(flows, i);
		const QString id = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID));
		const char *kind = obs_data_get_string(flow, WARP_FLOW_KIND);
		const bool enabled = obs_data_get_bool(flow, WARP_FLOW_ENABLED);
		const int max_clips = (int)obs_data_get_int(flow, WARP_FLOW_MAX_CLIPS);

		auto *item = new QTreeWidgetItem(tree);

		item->setIcon(WARP_COL_NAME, warp_flow_icon(kind));
		item->setText(WARP_COL_NAME, QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME)));
		item->setData(WARP_COL_NAME, Qt::UserRole, id);
		item->setData(WARP_COL_CLIPS, Qt::UserRole, max_clips);
		item->setText(WARP_COL_KIND, warp_flow_kind_name(kind));
		item->setText(WARP_COL_TARGET, QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_TARGET_NAME)));
		item->setText(WARP_COL_TRIGGER, warp_trigger_name(obs_data_get_string(flow, WARP_FLOW_TRIGGER)));
		item->setText(WARP_COL_ORDER, warp_order_name(obs_data_get_string(flow, WARP_FLOW_ORDER)));

		QStringList linked;
		obs_data_array_t *links = obs_data_get_array(flow, WARP_FLOW_LINKS);
		const size_t link_count = links ? obs_data_array_count(links) : 0;

		for (size_t j = 0; j < link_count; j++) {
			obs_data_t *link = obs_data_array_item(links, j);
			const QString link_id = QString::fromUtf8(obs_data_get_string(link, WARP_FLOW_LINK_ID));

			linked.append(names.value(link_id, link_id));
			obs_data_release(link);
		}

		obs_data_array_release(links);
		item->setText(WARP_COL_LINKS, linked.join(QStringLiteral(", ")));

		/* a flow that is switched off still passes clips on to the
		 * ones it is linked to; it just takes none itself */
		if (!enabled) {
			const QColor grey = QApplication::palette().color(QPalette::Disabled, QPalette::WindowText);

			for (int column = 0; column < WARP_COL_COUNT; column++)
				item->setForeground(column, grey);

			item->setText(WARP_COL_KIND, warp_flow_kind_name(kind) + QStringLiteral(" ") +
							     warp_flow_text("Warp.Window.Disabled"));
		}

		if (id == selected)
			tree->setCurrentItem(item);

		obs_data_release(flow);
	}

	obs_data_array_release(flows);

	selectionChanged();
	refreshStatus();
}

void WarpWindow::refreshStatus()
{
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = tree->topLevelItem(i);
		const QString id = item->data(WARP_COL_NAME, Qt::UserRole).toString();
		const int max_clips = item->data(WARP_COL_CLIPS, Qt::UserRole).toInt();
		const int clips = warp_flow_clip_count(id.toUtf8().constData());

		if (clips < 0)
			item->setText(WARP_COL_CLIPS, warp_flow_text("Warp.Window.TargetMissing"));
		else if (max_clips > 0)
			item->setText(WARP_COL_CLIPS, QString("%1 / %2").arg(clips).arg(max_clips));
		else
			item->setText(WARP_COL_CLIPS, QString::number(clips));
	}

	QString text = warp_flow_replay_buffer_active() ? warp_flow_text("Warp.Window.Buffer.Running")
							: warp_flow_text("Warp.Window.Buffer.Stopped");

	char *last = warp_flow_last_clip();

	if (last) {
		text += QStringLiteral("  -  ") +
			warp_flow_text("Warp.Window.LastClip").arg(QFileInfo(QString::fromUtf8(last)).fileName());
		bfree(last);
	} else {
		text += QStringLiteral("  -  ") + warp_flow_text("Warp.Window.LastClip.None");
	}

	status->setText(text);
}

void WarpWindow::addFlow()
{
	WarpFlowCreateDialog dialog(this);

	if (dialog.exec() != QDialog::Accepted)
		return;

	refresh();

	const QStringList made = dialog.createdIds();

	if (!made.isEmpty()) {
		for (int i = 0; i < tree->topLevelItemCount(); i++) {
			QTreeWidgetItem *item = tree->topLevelItem(i);

			if (item->data(WARP_COL_NAME, Qt::UserRole).toString() == made.last()) {
				tree->setCurrentItem(item);
				break;
			}
		}
	}
}

void WarpWindow::editFlow()
{
	const QString id = selectedId();

	if (id.isEmpty())
		return;

	WarpFlowPropsDialog dialog(this, id);

	if (dialog.exec() == QDialog::Accepted)
		refresh();
}

void WarpWindow::removeFlow()
{
	QTreeWidgetItem *item = tree->currentItem();
	const QString id = selectedId();

	if (id.isEmpty() || !item)
		return;

	const QString name = item->text(WARP_COL_NAME);
	const QMessageBox::StandardButton answer =
		QMessageBox::question(this, warp_flow_text("Warp.Window.Remove.Title"),
				      warp_flow_text("Warp.Window.Remove.Confirm").arg(name),
				      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

	if (answer != QMessageBox::Yes)
		return;

	warp_flow_remove(id.toUtf8().constData());
	refresh();
}

QPointer<WarpWindow> warp_window;

void show_warp_window(void *)
{
	if (!warp_window) {
		auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

		warp_window = new WarpWindow(main_window);
	} else {
		warp_window->refresh();
	}

	warp_window->show();
	warp_window->raise();
	warp_window->activateWindow();
}

} // namespace

extern "C" void warp_register_tools_menu(void)
{
	obs_frontend_add_tools_menu_item(obs_module_text("Warp.Tools.Menu"), show_warp_window, nullptr);
}
