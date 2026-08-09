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
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
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

/* how narrow a column of the flow list is allowed to get, whatever is in it */
constexpr int WARP_COLUMN_MIN_WIDTH = 100;

/* The kind column is nothing but the flow's icon. The view lays a decoration
 * down the left of the cell, which under a centred header would leave it
 * stranded, so it is drawn in the middle of the cell here instead. */
class WarpKindDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QStyleOptionViewItem opt = option;

		initStyleOption(&opt, index);

		const QIcon icon = opt.icon;

		/* the row itself first - its background, and whether it is the
		 * selected one - with the icon put down over it afterwards */
		opt.icon = QIcon();
		opt.features.setFlag(QStyleOptionViewItem::HasDecoration, false);
		opt.text.clear();

		QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();

		style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

		if (icon.isNull())
			return;

		/* a flow that is switched off is greyed out like the rest of its
		 * row is */
		const bool enabled = index.data(Qt::UserRole).toBool();
		QRect box(QPoint(0, 0), opt.decorationSize);

		box.moveCenter(opt.rect.center());
		icon.paint(painter, box, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
	}
};

/* the replay buffer, said as a dot: green while it is running, red while it is
 * not */
QIcon warp_buffer_icon(bool active)
{
	const int size = 64;
	QPixmap pixmap(size, size);

	pixmap.fill(Qt::transparent);

	QPainter p(&pixmap);

	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(Qt::NoPen);
	p.setBrush(active ? QColor(0x3E, 0xC7, 0x6A) : QColor(0xD2, 0x4B, 0x4B));
	p.drawEllipse(QRectF(size * 0.08, size * 0.08, size * 0.84, size * 0.84));
	p.end();

	return QIcon(pixmap);
}

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
	QLabel *bufferDot = nullptr;
	QLabel *status = nullptr;
	QTimer *timer = nullptr;

	/* what the dot is showing, so it is only drawn again when the replay
	 * buffer has actually changed its mind: -1 until it has been drawn once */
	int bufferShown = -1;
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
	/* the kind column is the only one carrying an icon, and the icon is all
	 * it carries, so it is drawn a size up from a decoration beside text */
	tree->setIconSize(QSize(24, 24));
	tree->setHeaderLabels({warp_flow_text("Warp.Window.Column.Flow"), warp_flow_text("Warp.Window.Column.Kind"),
			       warp_flow_text("Warp.Window.Column.Target"), warp_flow_text("Warp.Window.Column.Clips"),
			       warp_flow_text("Warp.Window.Column.Trigger"), warp_flow_text("Warp.Window.Column.Order"),
			       warp_flow_text("Warp.Window.Column.Links")});
	tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
	tree->header()->setStretchLastSection(true);
	tree->header()->setMinimumSectionSize(WARP_COLUMN_MIN_WIDTH);
	tree->header()->setDefaultAlignment(Qt::AlignCenter);
	tree->setItemDelegateForColumn(WARP_COL_KIND, new WarpKindDelegate(tree));

	auto *addButton = new QPushButton(warp_flow_text("Warp.Window.Add"), this);

	propsButton = new QPushButton(warp_flow_text("Warp.Window.Props"), this);
	removeButton = new QPushButton(warp_flow_text("Warp.Window.Remove"), this);

	auto *closeButton = new QPushButton(warp_flow_text("Warp.Window.Close"), this);

	closeButton->setDefault(true);

	bufferDot = new QLabel(this);
	status = new QLabel(this);
	status->setTextFormat(Qt::PlainText);

	auto *statusRow = new QHBoxLayout();

	statusRow->setContentsMargins(0, 0, 0, 0);
	statusRow->addWidget(bufferDot);
	statusRow->addWidget(status);
	statusRow->addStretch(1);

	auto *buttons = new QHBoxLayout();

	buttons->addWidget(addButton);
	buttons->addWidget(propsButton);
	buttons->addWidget(removeButton);
	buttons->addStretch(1);
	buttons->addWidget(closeButton);

	auto *layout = new QVBoxLayout(this);

	layout->addWidget(intro);
	layout->addWidget(tree, 1);
	layout->addLayout(statusRow);
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
		const QString name = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME));

		auto *item = new QTreeWidgetItem(tree);

		item->setText(WARP_COL_NAME, name);
		item->setData(WARP_COL_NAME, Qt::UserRole, id);
		item->setData(WARP_COL_CLIPS, Qt::UserRole, max_clips);

		/* the kind is the icon, and the icon is all of it: what it stands
		 * for is a hover away for anyone it is new to */
		item->setIcon(WARP_COL_KIND, warp_flow_icon(kind));
		item->setData(WARP_COL_KIND, Qt::UserRole, enabled);
		item->setToolTip(WARP_COL_KIND, warp_flow_kind_name(kind));

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

			item->setText(WARP_COL_NAME,
				      name + QStringLiteral(" ") + warp_flow_text("Warp.Window.Disabled"));
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

	const bool active = warp_flow_replay_buffer_active();

	if (bufferShown != (int)active) {
		bufferShown = (int)active;

		const int size = bufferDot->fontMetrics().height() * 3 / 4;

		bufferDot->setPixmap(
			warp_buffer_icon(active).pixmap(QSize(size, size), bufferDot->devicePixelRatioF()));
		bufferDot->setToolTip(active ? warp_flow_text("Warp.Window.Buffer.Running")
					     : warp_flow_text("Warp.Window.Buffer.Stopped"));
	}

	char *last = warp_flow_last_clip();

	if (last) {
		status->setText(
			warp_flow_text("Warp.Window.LastClip").arg(QFileInfo(QString::fromUtf8(last)).fileName()));
		bfree(last);
	} else {
		status->setText(warp_flow_text("Warp.Window.LastClip.None"));
	}
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

/* one prompt at a time, however many times the hotkey is leant on */
bool warp_buffer_prompt_open = false;

/* A flow's Save Replay hotkey, pressed with no replay buffer running to save.
 * Told from the UI thread, but the dialog is left to the event loop rather
 * than opened inside the task the press is being carried out in. */
void warp_window_buffer_prompt(const char *flow_name)
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	if (!main_window || warp_buffer_prompt_open)
		return;

	warp_buffer_prompt_open = true;

	const QString name = QString::fromUtf8(flow_name ? flow_name : "");

	QMetaObject::invokeMethod(
		main_window,
		[main_window, name]() {
			QMessageBox box(main_window);

			box.setWindowTitle(warp_flow_text("Warp.Flow.Buffer.Title"));
			box.setIcon(QMessageBox::Warning);
			box.setText(warp_flow_text("Warp.Flow.Buffer.Text").arg(name));
			box.setInformativeText(warp_flow_text("Warp.Flow.Buffer.Info"));

			QPushButton *start =
				box.addButton(warp_flow_text("Warp.Flow.Buffer.Start"), QMessageBox::AcceptRole);

			box.addButton(warp_flow_text("Warp.Flow.Buffer.Cancel"), QMessageBox::RejectRole);
			box.setDefaultButton(start);
			box.exec();

			warp_buffer_prompt_open = false;

			/* the buffer may well have been started from elsewhere
			 * while the question was sitting there */
			if (box.clickedButton() == start && !obs_frontend_replay_buffer_active())
				obs_frontend_replay_buffer_start();
		},
		Qt::QueuedConnection);
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
	warp_flow_set_buffer_prompt(warp_window_buffer_prompt);
}
