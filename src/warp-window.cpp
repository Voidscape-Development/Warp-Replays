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
#include <functional>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include "warp-events.h"
#include "warp-flow-dialog.hpp"
#include "warp-flow.h"
#include "warp-status.h"
#include "warp-zoom-dock.hpp"

namespace {

/* The flow list reads left to right as what an operator asks of a flow: is it
 * on, which one is it, where do its clips go, and how many are there. Settings
 * that are set once and rarely looked at again are said as chips under the
 * name, and in full in the panel beside the list. */
enum WarpFlowColumn {
	WARP_COL_ON,
	WARP_COL_FLOW,
	WARP_COL_FEEDS,
	WARP_COL_CLIPS,
	WARP_COL_ACTIONS,
	WARP_COL_COUNT,
};

/* what the delegate draws a row from, kept on the column it belongs to */
enum WarpFlowRole {
	/* the flow column */
	WARP_ROLE_ID = Qt::UserRole,
	WARP_ROLE_KIND,
	WARP_ROLE_ENABLED,
	WARP_ROLE_CHIPS,
	WARP_ROLE_LINKS,
	/* the feeds column, under the source's name */
	WARP_ROLE_SUBTITLE,
	/* the feeds and clips columns: the source the flow feeds is gone */
	WARP_ROLE_MISSING,
	/* the clips column */
	WARP_ROLE_CLIPS,
	WARP_ROLE_MAX_CLIPS,
	WARP_ROLE_INSTANT,
};

/* the room around what is drawn in a cell, and between its two lines */
constexpr int WARP_CELL_MARGIN = 6;
constexpr int WARP_ROW_PADDING = 7;
constexpr int WARP_LINE_GAP = 3;
/* each step a flow fed by another is set in under it */
constexpr int WARP_INDENT = 28;
constexpr int WARP_ICON_SIZE = 24;

/* The colours that say something rather than decorate: running, stopped or
 * gone, and a list at its limit. The same green and red the replay buffer dot
 * has always been, and they read on the light themes as well as the dark. */
const QColor warp_green(0x3E, 0xC7, 0x6A);
const QColor warp_red(0xD2, 0x4B, 0x4B);
const QColor warp_amber(0xE0, 0xA0, 0x30);

QColor warp_with_alpha(QColor color, qreal alpha)
{
	color.setAlphaF(alpha);
	return color;
}

/* the colour secondary text is set in: the theme's own text, let back far
 * enough to read as second to it on a dark theme and a light one alike */
QColor warp_dim_color()
{
	return warp_with_alpha(QApplication::palette().color(QPalette::WindowText), 0.62);
}

QString warp_color_css(const QColor &color)
{
	return QStringLiteral("rgba(%1, %2, %3, %4)")
		.arg(color.red())
		.arg(color.green())
		.arg(color.blue())
		.arg(color.alpha());
}

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
	p.setBrush(active ? warp_green : warp_red);
	p.drawEllipse(QRectF(size * 0.08, size * 0.08, size * 0.84, size * 0.84));
	p.end();

	return QIcon(pixmap);
}

/* ------------------------------------------------------------------------- */
/* what a flow is, said short */

QString warp_length_label(int seconds)
{
	char label[WARP_FLOW_LENGTH_LABEL_SIZE];

	warp_flow_length_label(seconds, label, sizeof(label));

	return QString::fromUtf8(label);
}

/* the lengths a flow offers on top of its own, in seconds */
QList<int> warp_other_lengths(obs_data_t *flow)
{
	QList<int> lengths;
	obs_data_array_t *array = obs_data_get_array(flow, WARP_FLOW_LENGTHS);
	const size_t count = array ? obs_data_array_count(array) : 0;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);

		lengths.append((int)obs_data_get_int(item, WARP_FLOW_LENGTH_SECONDS));
		obs_data_release(item);
	}

	obs_data_array_release(array);

	return lengths;
}

/* the ids of the flows this one hands its clips on to */
QStringList warp_links(obs_data_t *flow)
{
	QStringList links;
	obs_data_array_t *array = obs_data_get_array(flow, WARP_FLOW_LINKS);
	const size_t count = array ? obs_data_array_count(array) : 0;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);

		links.append(QString::fromUtf8(obs_data_get_string(item, WARP_FLOW_LINK_ID)));
		obs_data_release(item);
	}

	obs_data_array_release(array);

	return links;
}

bool warp_is_instant(const char *kind)
{
	return kind && strcmp(kind, WARP_FLOW_KIND_INSTANT) == 0;
}

/* How much of the replay buffer a flow keeps, with the lengths it offers on top
 * of that after it: "10s +5s, 20s". */
QString warp_length_chip(obs_data_t *flow)
{
	const int length = (int)obs_data_get_int(flow, WARP_FLOW_LENGTH);
	const QString own = length > 0 ? warp_length_label(length) : warp_flow_text("Warp.Window.Length.Whole");
	QStringList others;

	for (int seconds : warp_other_lengths(flow))
		others.append(warp_length_label(seconds));

	if (others.isEmpty())
		return own;

	return warp_flow_text("Warp.Window.Length.Others").arg(own, others.join(QStringLiteral(", ")));
}

/* What a flow does with a clip, in a word or two each: where its clips come
 * from, which end of its list they go on - or, for an instant replay, which
 * holds one clip rather than a list, what playback does as it lands - and how
 * much of the buffer it keeps. */
QStringList warp_chips(obs_data_t *flow)
{
	const char *kind = obs_data_get_string(flow, WARP_FLOW_KIND);
	const char *trigger = obs_data_get_string(flow, WARP_FLOW_TRIGGER);
	QStringList chips;

	chips.append(trigger && strcmp(trigger, WARP_FLOW_TRIGGER_LISTEN) == 0
			     ? warp_flow_text("Warp.Window.Chip.Listen")
			     : warp_flow_text("Warp.Window.Chip.Hotkey"));

	if (warp_is_instant(kind)) {
		const char *playback = obs_data_get_string(flow, WARP_FLOW_PLAYBACK);
		const int speed = (int)obs_data_get_int(flow, WARP_FLOW_SPEED);
		QString chip = warp_flow_text("Warp.Window.Chip.Keep");

		if (playback && strcmp(playback, WARP_MEDIA_LOAD_PLAY) == 0)
			chip = warp_flow_text("Warp.Window.Chip.Play");
		else if (playback && strcmp(playback, WARP_MEDIA_LOAD_HOLD) == 0)
			chip = warp_flow_text("Warp.Window.Chip.Hold");

		if (speed > 0)
			chip = warp_flow_text("Warp.Window.Chip.Speed").arg(chip).arg(speed);

		chips.append(chip);
	} else {
		const char *order = obs_data_get_string(flow, WARP_FLOW_ORDER);

		chips.append(order && strcmp(order, WARP_FLOW_ORDER_NEWEST_FIRST) == 0
				     ? warp_flow_text("Warp.Flow.Order.NewestFirst")
				     : warp_flow_text("Warp.Flow.Order.OldestFirst"));
	}

	chips.append(warp_length_chip(flow));

	return chips;
}

QString warp_trigger_name(const char *trigger)
{
	if (trigger && strcmp(trigger, WARP_FLOW_TRIGGER_LISTEN) == 0)
		return warp_flow_text("Warp.Flow.Trigger.Listen");

	return warp_flow_text("Warp.Flow.Trigger.Hotkey");
}

QString warp_playback_name(const char *playback)
{
	if (playback && strcmp(playback, WARP_MEDIA_LOAD_PLAY) == 0)
		return warp_flow_text("Warp.Flow.Playback.Play");
	if (playback && strcmp(playback, WARP_MEDIA_LOAD_HOLD) == 0)
		return warp_flow_text("Warp.Flow.Playback.Hold");

	return warp_flow_text("Warp.Flow.Playback.Keep");
}

/* ------------------------------------------------------------------------- */
/* hotkeys */

/* Every key bound to anything, by the hotkey it is bound to, as the hotkey
 * settings would write it: "Shift+F9". A hotkey bound to more than one key has
 * them all, in the order they were bound. */
QMap<obs_hotkey_id, QStringList> warp_bound_keys()
{
	QMap<obs_hotkey_id, QStringList> keys;

	obs_enum_hotkey_bindings(
		[](void *data, size_t, obs_hotkey_binding_t *binding) {
			auto *map = static_cast<QMap<obs_hotkey_id, QStringList> *>(data);
			const obs_key_combination_t combination = obs_hotkey_binding_get_key_combination(binding);
			struct dstr text = {};

			obs_key_combination_to_str(combination, &text);

			if (text.array && *text.array)
				(*map)[obs_hotkey_binding_get_hotkey_id(binding)].append(QString::fromUtf8(text.array));

			dstr_free(&text);
			return true;
		},
		&keys);

	return keys;
}

QString warp_key_text(const QMap<obs_hotkey_id, QStringList> &keys, obs_hotkey_id id)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return QString();

	return keys.value(id).join(QStringLiteral(" / "));
}

/* ------------------------------------------------------------------------- */
/* how long ago */

QString warp_ago(uint64_t then_ns)
{
	const uint64_t now = os_gettime_ns();
	const long long seconds = now > then_ns ? (long long)((now - then_ns) / 1000000000ULL) : 0;

	if (seconds < 10)
		return warp_flow_text("Warp.Window.Ago.Now");
	if (seconds < 60)
		return warp_flow_text("Warp.Window.Ago.Seconds").arg(seconds);
	if (seconds < 3600)
		return warp_flow_text("Warp.Window.Ago.Minutes").arg(seconds / 60);

	return warp_flow_text("Warp.Window.Ago.Hours").arg(seconds / 3600);
}

/* ------------------------------------------------------------------------- */
/* the switch a flow is turned on and off with */

class WarpSwitch : public QAbstractButton {
public:
	explicit WarpSwitch(QWidget *parent) : QAbstractButton(parent)
	{
		setCheckable(true);
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::StrongFocus);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	}

	QSize sizeHint() const override { return QSize(36, 20); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);

		p.setRenderHint(QPainter::Antialiasing, true);

		const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
		const qreal radius = track.height() / 2.0;
		const QColor ink = palette().color(QPalette::WindowText);

		p.setPen(Qt::NoPen);
		p.setBrush(isChecked() ? warp_green : warp_with_alpha(ink, 0.25));
		p.drawRoundedRect(track, radius, radius);

		const qreal knob = track.height() - 4.0;
		const qreal x = isChecked() ? track.right() - 2.0 - knob : track.left() + 2.0;

		p.setBrush(isChecked() ? QColor(Qt::white) : warp_with_alpha(ink, 0.7));
		p.drawEllipse(QRectF(x, track.top() + 2.0, knob, knob));

		if (hasFocus()) {
			QPen pen(palette().color(QPalette::Highlight));

			pen.setWidthF(1.5);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(track, radius, radius);
		}
	}
};

/* ------------------------------------------------------------------------- */
/* the flow list */

/* Draws the flow, feeds and clips cells: two lines each, the thing itself on
 * top and what it is underneath. A flow that is switched off is drawn faded,
 * like the rest of its row. */
class WarpFlowDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	static QFont smallFont(const QFont &font)
	{
		QFont small = font;

		if (font.pointSizeF() > 0)
			small.setPointSizeF(font.pointSizeF() * 0.85);
		else if (font.pixelSize() > 0)
			small.setPixelSize(qMax(8, font.pixelSize() * 85 / 100));

		return small;
	}

	static int chipHeight(const QFont &font) { return QFontMetrics(smallFont(font)).height() + 2; }

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QSize size = QStyledItemDelegate::sizeHint(option, index);
		QFont bold = option.font;

		bold.setBold(true);

		const int lines = QFontMetrics(bold).height() + WARP_LINE_GAP + chipHeight(option.font);

		size.setHeight(qMax(size.height(), qMax(lines, WARP_ICON_SIZE) + WARP_ROW_PADDING * 2));

		return size;
	}

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QStyleOptionViewItem opt = option;

		initStyleOption(&opt, index);

		const QIcon icon = opt.icon;
		const QString text = opt.text;

		/* the row itself first - its background, and whether it is the
		 * selected one - with the cell drawn over it afterwards */
		opt.icon = QIcon();
		opt.features.setFlag(QStyleOptionViewItem::HasDecoration, false);
		opt.text.clear();

		QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();

		style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

		const bool selected = opt.state.testFlag(QStyle::State_Selected);
		const QColor ink =
			opt.palette.color(QPalette::Normal, selected ? QPalette::HighlightedText : QPalette::Text);
		const bool enabled = index.siblingAtColumn(WARP_COL_FLOW).data(WARP_ROLE_ENABLED).toBool();
		const QRect cell =
			opt.rect.adjusted(WARP_CELL_MARGIN, WARP_ROW_PADDING, -WARP_CELL_MARGIN, -WARP_ROW_PADDING);

		painter->save();
		painter->setRenderHint(QPainter::Antialiasing, true);

		if (!enabled)
			painter->setOpacity(0.5);

		switch (index.column()) {
		case WARP_COL_FLOW:
			paintFlow(painter, opt, index, cell, ink, icon, text);
			break;
		case WARP_COL_FEEDS:
			paintFeeds(painter, opt, index, cell, ink, selected, text);
			break;
		case WARP_COL_CLIPS:
			paintClips(painter, opt, index, cell, ink, selected);
			break;
		default:
			break;
		}

		painter->restore();
	}

private:
	/* the name on top, bold, and the chips under it */
	void paintFlow(QPainter *painter, const QStyleOptionViewItem &opt, const QModelIndex &index, const QRect &cell,
		       const QColor &ink, const QIcon &icon, const QString &name) const
	{
		const int iconSize = qMin(cell.height(), WARP_ICON_SIZE);
		QRect iconBox(cell.left(), cell.center().y() - iconSize / 2, iconSize, iconSize);

		icon.paint(painter, iconBox, Qt::AlignCenter);

		const QRect text = cell.adjusted(iconSize + 8, 0, 0, 0);
		QFont bold = opt.font;

		bold.setBold(true);

		const QFontMetrics boldMetrics(bold);
		const QRect nameRect(text.left(), text.top(), text.width(), boldMetrics.height());

		painter->setFont(bold);
		painter->setPen(ink);
		painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
				  boldMetrics.elidedText(name, Qt::ElideRight, nameRect.width()));

		const QFont small = smallFont(opt.font);
		const QFontMetrics smallMetrics(small);
		const int height = chipHeight(opt.font);
		const int padding = 5;
		int x = text.left();
		const int y = nameRect.bottom() + 1 + WARP_LINE_GAP;

		painter->setFont(small);

		auto chip = [&](const QString &label, const QColor &fill) {
			const int room = text.right() + 1 - x;

			if (room < padding * 2 + smallMetrics.horizontalAdvance(QStringLiteral("..."))) {
				x = text.right() + 1;
				return;
			}

			const QString shown = smallMetrics.elidedText(label, Qt::ElideRight, room - padding * 2);
			const QRect box(x, y, smallMetrics.horizontalAdvance(shown) + padding * 2, height);

			painter->setPen(Qt::NoPen);
			painter->setBrush(fill);
			painter->drawRoundedRect(box, 3, 3);
			painter->setPen(ink);
			painter->drawText(box, Qt::AlignCenter, shown);

			x = box.right() + 1 + 4;
		};

		for (const QString &label : index.data(WARP_ROLE_CHIPS).toStringList())
			chip(label, warp_with_alpha(ink, 0.12));

		/* where its clips go on to, in amber: the one thing about a flow
		 * that is about other flows */
		for (const QString &link : index.data(WARP_ROLE_LINKS).toStringList())
			chip(warp_flow_text("Warp.Window.Chip.Link").arg(link), warp_with_alpha(warp_amber, 0.30));
	}

	/* the source on top, and what kind of source it is underneath; a source
	 * that has gone is said in red */
	void paintFeeds(QPainter *painter, const QStyleOptionViewItem &opt, const QModelIndex &index, const QRect &cell,
			const QColor &ink, bool selected, const QString &source) const
	{
		const bool missing = index.data(WARP_ROLE_MISSING).toBool();
		QFont top = opt.font;

		top.setBold(missing);

		const QFontMetrics topMetrics(top);
		const QRect topRect(cell.left(), cell.top(), cell.width(), topMetrics.height());

		painter->setFont(top);
		painter->setPen(missing && !selected ? warp_red : ink);
		painter->drawText(topRect, Qt::AlignLeft | Qt::AlignVCenter,
				  topMetrics.elidedText(missing ? warp_flow_text("Warp.Window.Feeds.Missing") : source,
							Qt::ElideRight, topRect.width()));

		const QFont small = smallFont(opt.font);
		const QFontMetrics smallMetrics(small);
		const QRect bottomRect(cell.left(), topRect.bottom() + 1 + WARP_LINE_GAP, cell.width(),
				       smallMetrics.height());
		const QString subtitle = missing ? warp_flow_text("Warp.Window.Feeds.Gone").arg(source)
						 : index.data(WARP_ROLE_SUBTITLE).toString();

		painter->setFont(small);
		painter->setPen(warp_with_alpha(ink, 0.6));
		painter->drawText(bottomRect, Qt::AlignLeft | Qt::AlignVCenter,
				  smallMetrics.elidedText(subtitle, Qt::ElideRight, bottomRect.width()));
	}

	/* the count, and for a list with a limit, a bar of how full it is */
	void paintClips(QPainter *painter, const QStyleOptionViewItem &opt, const QModelIndex &index, const QRect &cell,
			const QColor &ink, bool selected) const
	{
		const bool missing = index.data(WARP_ROLE_MISSING).toBool();
		const bool instant = index.data(WARP_ROLE_INSTANT).toBool();
		const int clips = index.data(WARP_ROLE_CLIPS).toInt();
		const int max = index.data(WARP_ROLE_MAX_CLIPS).toInt();
		const QFontMetrics metrics(opt.font);
		QString text;

		if (missing)
			text = QStringLiteral("-");
		else if (instant)
			text = clips > 0 ? warp_flow_text("Warp.Window.Clips.Loaded")
					 : warp_flow_text("Warp.Window.Clips.Empty");
		else if (max > 0)
			text = QStringLiteral("%1 / %2").arg(clips).arg(max);
		else
			text = QString::number(clips);

		const bool meter = !missing && !instant && max > 0;
		const int meterHeight = 4;
		const int block = metrics.height() + (meter ? WARP_LINE_GAP + meterHeight : 0);
		const QRect textRect(cell.left(), cell.center().y() - block / 2, cell.width(), metrics.height());

		painter->setFont(opt.font);
		painter->setPen(missing && !selected ? warp_red : ink);
		painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
				  metrics.elidedText(text, Qt::ElideRight, textRect.width()));

		if (!meter)
			return;

		const QRectF track(cell.left(), textRect.bottom() + 1 + WARP_LINE_GAP, cell.width(), meterHeight);
		const qreal share = qBound(0.0, (qreal)clips / (qreal)max, 1.0);
		const bool full = clips >= max;
		const QColor fill = full ? warp_amber
					 : (selected ? ink : opt.palette.color(QPalette::Normal, QPalette::Highlight));

		painter->setPen(Qt::NoPen);
		painter->setBrush(warp_with_alpha(ink, 0.15));
		painter->drawRoundedRect(track, 2, 2);

		if (share > 0.0) {
			painter->setBrush(fill);
			painter->drawRoundedRect(
				QRectF(track.left(), track.top(), track.width() * share, track.height()), 2, 2);
		}
	}
};

/* The flow list. A flow fed by another is set in under it with a line from the
 * one feeding it, so a replay list and the highlight list it fills read as the
 * pair they are rather than as two rows that happen to share a word. */
class WarpFlowTree : public QTreeWidget {
public:
	using QTreeWidget::QTreeWidget;

protected:
	void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override
	{
		if (!index.parent().isValid() || rect.width() < WARP_INDENT)
			return;

		/* down from the middle of the icon of the flow above, and across
		 * to this one's */
		const int x = rect.right() + 1 - WARP_INDENT + WARP_CELL_MARGIN + WARP_ICON_SIZE / 2;
		const int y = rect.center().y();
		QPen pen(warp_with_alpha(palette().color(QPalette::Text), 0.35));

		pen.setWidthF(1.5);

		painter->save();
		painter->setRenderHint(QPainter::Antialiasing, true);
		painter->setPen(pen);
		painter->setBrush(Qt::NoBrush);

		QPainterPath path;

		path.moveTo(x, rect.top());
		path.lineTo(x, y - 6);
		path.quadTo(x, y, x + 6, y);
		path.lineTo(rect.right() + 1, y);
		painter->drawPath(path);
		painter->restore();
	}
};

/* ------------------------------------------------------------------------- */
/* the window */

void warp_window_buffer_prompt(const char *flow_name);

/* The Warp window: the flows of the scene collection, and what is being made
 * of them.
 *
 * Laid out for the middle of a show as much as for setting one up. The replay
 * buffer, which no flow can do anything without, is along the top with the
 * button that starts it; the clip that was saved last, and where it went, is
 * along the bottom; and each flow can be switched off, or saved to, from its own
 * row. Everything about the flow that is picked is in the panel beside the
 * list, the keys that run it included.
 *
 * Everything that changes a flow is done from here, so the list is rebuilt
 * after every one of them rather than watched for. */
class WarpWindow : public QDialog {
public:
	explicit WarpWindow(QWidget *parent);
	~WarpWindow() override;

	/* the flow list from the top */
	void refresh();
	/* what does not need the list rebuilt: the replay buffer, how many clips
	 * each flow's source is holding, the keys bound to them, and the clip
	 * that was saved last */
	void refreshStatus();

protected:
	void changeEvent(QEvent *event) override;

private:
	/* Runs 'work' once the event that asked for it is over. The list and the
	 * panel are built again after anything is done to a flow, and the button
	 * that was pressed is one of the things that go, so nothing is done from
	 * inside its own click. */
	void later(std::function<void()> work);

	QTreeWidgetItem *addRow(QTreeWidgetItem *under, obs_data_t *flow, const QMap<QString, QString> &names);
	QWidget *makeSwitch(const QString &id, const QString &name, bool enabled);
	QWidget *makeActions(const QString &id, const QString &name, const QList<int> &lengths);
	QMenu *makeMenu(QWidget *parent, const QString &id, const QList<int> &lengths, bool showSave);

	void addFlow();
	void editFlow(const QString &id);
	void removeFlow(const QString &id);
	void setFlowEnabled(const QString &id, bool enabled);
	void saveReplay(const QString &id, int seconds);
	void addLastClip(const QString &id);
	void toggleBuffer();
	void showHelp();

	void selectionChanged();
	QString selectedId() const;
	void selectId(const QString &id);
	QTreeWidgetItem *itemFor(const QString &id) const;

	void buildInspector();
	void refreshBuffer();
	void refreshFooter();

	WarpFlowTree *tree = nullptr;
	QStackedWidget *listStack = nullptr;
	QLabel *countLabel = nullptr;

	QFrame *bufferPill = nullptr;
	QLabel *bufferDot = nullptr;
	QLabel *bufferLabel = nullptr;
	QPushButton *bufferButton = nullptr;
	QFrame *banner = nullptr;

	QScrollArea *inspector = nullptr;
	QLabel *footer = nullptr;
	QTimer *timer = nullptr;

	/* the Save button of each row, by flow, so the key on it can be kept up
	 * to date without building the list again */
	QMap<QString, QPointer<QToolButton>> saveButtons;
	/* the names of the flows that hand their clips to each flow */
	QMap<QString, QStringList> fedBy;

	/* what the replay buffer was last shown as, so it is only drawn again
	 * when it has actually changed its mind: -1 until it has been drawn */
	int bufferShown = -1;
	/* whether the source of the flow in the panel was missing when the
	 * panel was built, so the panel is built again when that changes */
	int inspectorMissing = -1;
};

void warp_window_frontend_event(enum obs_frontend_event event, void *data)
{
	auto *window = static_cast<WarpWindow *>(data);

	switch (event) {
	/* the flows belong to the scene collection, so they are not the ones
	 * that were on screen a moment ago */
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		QMetaObject::invokeMethod(window, [window]() { window->refresh(); }, Qt::QueuedConnection);
		break;
	/* said at once rather than on the next tick of the timer, since it is
	 * what the button along the top was pressed for */
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		QMetaObject::invokeMethod(window, [window]() { window->refreshStatus(); }, Qt::QueuedConnection);
		break;
	default:
		break;
	}
}

QLabel *warp_section_label(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text.toUpper(), parent);
	QFont font = label->font();

	font.setBold(true);
	font.setLetterSpacing(QFont::PercentageSpacing, 108);

	if (font.pointSizeF() > 0)
		font.setPointSizeF(font.pointSizeF() * 0.8);

	label->setFont(font);
	label->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_dim_color())));

	return label;
}

QLabel *warp_value_label(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);

	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);

	return label;
}

QFrame *warp_rule(QWidget *parent)
{
	auto *rule = new QFrame(parent);

	rule->setFrameShape(QFrame::HLine);
	rule->setFrameShadow(QFrame::Plain);
	rule->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_with_alpha(warp_dim_color(), 0.4))));

	return rule;
}

WarpWindow::WarpWindow(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(warp_flow_text("Warp.Window.Title"));
	resize(1040, 580);
	setMinimumSize(560, 360);

	/* along the top: what this is and how to add to it on the left, the
	 * replay buffer and the tools that are not about any one flow on the
	 * right */
	auto *title = new QLabel(warp_flow_text("Warp.Window.Flows"), this);
	QFont titleFont = title->font();

	titleFont.setBold(true);

	if (titleFont.pointSizeF() > 0)
		titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);

	title->setFont(titleFont);

	countLabel = new QLabel(this);
	countLabel->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_dim_color())));

	auto *addButton = new QPushButton(warp_flow_text("Warp.Window.Add"), this);

	bufferPill = new QFrame(this);
	bufferPill->setObjectName(QStringLiteral("warpBufferPill"));
	bufferPill->setStyleSheet(QStringLiteral("QFrame#warpBufferPill { border: 1px solid %1; border-radius: 6px; }")
					  .arg(warp_color_css(warp_with_alpha(
						  QApplication::palette().color(QPalette::WindowText), 0.25))));

	bufferDot = new QLabel(bufferPill);
	bufferLabel = new QLabel(bufferPill);
	bufferButton = new QPushButton(bufferPill);

	auto *pillRow = new QHBoxLayout(bufferPill);

	pillRow->setContentsMargins(10, 3, 3, 3);
	pillRow->setSpacing(8);
	pillRow->addWidget(bufferDot);
	pillRow->addWidget(bufferLabel);
	pillRow->addWidget(bufferButton);

	auto *zoomButton = new QPushButton(warp_flow_text("Warp.Window.ZoomPresets"), this);

	zoomButton->setToolTip(warp_flow_text("Warp.Window.ZoomPresets.Desc"));

	auto *helpButton = new QToolButton(this);

	helpButton->setText(QStringLiteral("?"));
	helpButton->setToolTip(warp_flow_text("Warp.Window.Help"));
	helpButton->setAccessibleName(warp_flow_text("Warp.Window.Help"));

	auto *head = new QHBoxLayout();

	head->setSpacing(10);
	head->addWidget(title);
	head->addWidget(countLabel);
	head->addSpacing(6);
	head->addWidget(addButton);
	head->addStretch(1);
	head->addWidget(bufferPill);
	head->addWidget(zoomButton);
	head->addWidget(helpButton);

	/* across the whole window while the replay buffer is stopped, since
	 * nothing below can take a clip until it is started */
	banner = new QFrame(this);
	banner->setObjectName(QStringLiteral("warpBufferBanner"));
	banner->setStyleSheet(QStringLiteral("QFrame#warpBufferBanner { background-color: %1; border: 1px solid %2;"
					     " border-radius: 4px; }")
				      .arg(warp_color_css(warp_with_alpha(warp_red, 0.16)),
					   warp_color_css(warp_with_alpha(warp_red, 0.55))));

	auto *bannerIcon = new QLabel(banner);
	const int iconSize = bannerIcon->fontMetrics().height();

	bannerIcon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning)
				      .pixmap(QSize(iconSize, iconSize), devicePixelRatioF()));

	auto *bannerText = new QLabel(warp_flow_text("Warp.Window.Banner"), banner);

	bannerText->setWordWrap(true);
	bannerText->setTextFormat(Qt::PlainText);

	auto *bannerButton = new QPushButton(warp_flow_text("Warp.Window.Banner.Start"), banner);
	auto *bannerRow = new QHBoxLayout(banner);

	bannerRow->setContentsMargins(10, 6, 6, 6);
	bannerRow->setSpacing(10);
	bannerRow->addWidget(bannerIcon, 0, Qt::AlignTop);
	bannerRow->addWidget(bannerText, 1);
	bannerRow->addWidget(bannerButton);

	/* the list, or, with nothing in it, what it is for and how to start */
	tree = new WarpFlowTree(this);
	tree->setColumnCount(WARP_COL_COUNT);
	tree->setTreePosition(WARP_COL_FLOW);
	tree->setRootIsDecorated(false);
	tree->setItemsExpandable(false);
	tree->setIndentation(WARP_INDENT);
	tree->setSelectionMode(QAbstractItemView::SingleSelection);
	tree->setSelectionBehavior(QAbstractItemView::SelectRows);
	tree->setAllColumnsShowFocus(true);
	tree->setUniformRowHeights(true);
	tree->setIconSize(QSize(WARP_ICON_SIZE, WARP_ICON_SIZE));
	tree->setItemDelegate(new WarpFlowDelegate(tree));
	tree->setHeaderLabels({warp_flow_text("Warp.Window.Column.On"), warp_flow_text("Warp.Window.Column.Flow"),
			       warp_flow_text("Warp.Window.Column.Target"), warp_flow_text("Warp.Window.Column.Clips"),
			       QString()});

	QHeaderView *header = tree->header();

	header->setStretchLastSection(false);
	header->setSectionsMovable(false);
	header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	header->setSectionResizeMode(WARP_COL_ON, QHeaderView::Fixed);
	header->setSectionResizeMode(WARP_COL_FLOW, QHeaderView::Stretch);
	header->setSectionResizeMode(WARP_COL_FEEDS, QHeaderView::Interactive);
	header->setSectionResizeMode(WARP_COL_CLIPS, QHeaderView::Interactive);
	header->setSectionResizeMode(WARP_COL_ACTIONS, QHeaderView::ResizeToContents);
	header->resizeSection(WARP_COL_ON, 52);
	header->resizeSection(WARP_COL_FEEDS, 160);
	header->resizeSection(WARP_COL_CLIPS, 88);

	auto *empty = new QWidget(this);
	auto *emptyTitle = new QLabel(warp_flow_text("Warp.Window.Empty.Title"), empty);

	emptyTitle->setFont(titleFont);
	emptyTitle->setAlignment(Qt::AlignCenter);

	auto *emptyText = new QLabel(warp_flow_text("Warp.Window.Intro"), empty);

	emptyText->setWordWrap(true);
	emptyText->setTextFormat(Qt::PlainText);
	emptyText->setAlignment(Qt::AlignCenter);
	emptyText->setMaximumWidth(520);

	auto *emptyAdd = new QPushButton(warp_flow_text("Warp.Window.Add"), empty);
	auto *emptyLayout = new QVBoxLayout(empty);

	emptyLayout->addStretch(1);
	emptyLayout->addWidget(emptyTitle, 0, Qt::AlignHCenter);
	emptyLayout->addWidget(emptyText, 0, Qt::AlignHCenter);
	emptyLayout->addSpacing(6);
	emptyLayout->addWidget(emptyAdd, 0, Qt::AlignHCenter);
	emptyLayout->addStretch(2);

	listStack = new QStackedWidget(this);
	listStack->addWidget(tree);
	listStack->addWidget(empty);

	/* beside the list, everything about the flow that is picked */
	inspector = new QScrollArea(this);
	inspector->setWidgetResizable(true);
	inspector->setFrameShape(QFrame::NoFrame);
	inspector->setMinimumWidth(260);
	inspector->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto *splitter = new QSplitter(Qt::Horizontal, this);

	splitter->addWidget(listStack);
	splitter->addWidget(inspector);
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 0);
	splitter->setCollapsible(0, false);
	splitter->setSizes({760, 280});

	/* along the bottom, the clip that was saved last and where it went */
	footer = new QLabel(this);
	footer->setTextFormat(Qt::RichText);
	footer->setWordWrap(true);
	footer->setTextInteractionFlags(Qt::TextSelectableByMouse);

	auto *layout = new QVBoxLayout(this);

	layout->setSpacing(8);
	layout->addLayout(head);
	layout->addWidget(banner);
	layout->addWidget(splitter, 1);
	layout->addWidget(warp_rule(this));
	layout->addWidget(footer);

	connect(addButton, &QPushButton::clicked, this, [this]() { later([this]() { addFlow(); }); });
	connect(emptyAdd, &QPushButton::clicked, this, [this]() { later([this]() { addFlow(); }); });
	connect(zoomButton, &QPushButton::clicked, this, [this]() { warp_open_zoom_presets(this); });
	connect(helpButton, &QToolButton::clicked, this, [this]() { showHelp(); });
	connect(bufferButton, &QPushButton::clicked, this, [this]() { toggleBuffer(); });
	connect(bannerButton, &QPushButton::clicked, this, []() {
		if (!obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_start();
	});
	connect(tree, &QTreeWidget::itemSelectionChanged, this, [this]() { selectionChanged(); });
	connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
		const QString id = item ? item->data(WARP_COL_FLOW, WARP_ROLE_ID).toString() : QString();

		later([this, id]() { editFlow(id); });
	});

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

void WarpWindow::later(std::function<void()> work)
{
	QTimer::singleShot(0, this, std::move(work));
}

void WarpWindow::changeEvent(QEvent *event)
{
	QDialog::changeEvent(event);

	/* coming back from Settings, the keys may well have been bound since */
	if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
		refreshStatus();
		buildInspector();
	}
}

QString WarpWindow::selectedId() const
{
	QTreeWidgetItem *item = tree->currentItem();

	return item ? item->data(WARP_COL_FLOW, WARP_ROLE_ID).toString() : QString();
}

QTreeWidgetItem *WarpWindow::itemFor(const QString &id) const
{
	for (QTreeWidgetItemIterator it(tree); *it; ++it) {
		if ((*it)->data(WARP_COL_FLOW, WARP_ROLE_ID).toString() == id)
			return *it;
	}

	return nullptr;
}

void WarpWindow::selectId(const QString &id)
{
	QTreeWidgetItem *item = itemFor(id);

	if (item)
		tree->setCurrentItem(item);
}

void WarpWindow::selectionChanged()
{
	buildInspector();
}

QWidget *WarpWindow::makeSwitch(const QString &id, const QString &name, bool enabled)
{
	auto *holder = new QWidget(tree);
	auto *toggle = new WarpSwitch(holder);
	auto *row = new QHBoxLayout(holder);

	row->setContentsMargins(0, 0, 0, 0);
	row->addWidget(toggle, 0, Qt::AlignCenter);

	toggle->setChecked(enabled);
	toggle->setToolTip(enabled ? warp_flow_text("Warp.Window.Switch.On").arg(name)
				   : warp_flow_text("Warp.Window.Switch.Off").arg(name));
	toggle->setAccessibleName(warp_flow_text("Warp.Window.Column.On") + QStringLiteral(": ") + name);

	connect(toggle, &QAbstractButton::toggled, this,
		[this, id](bool on) { later([this, id, on]() { setFlowEnabled(id, on); }); });

	return holder;
}

QMenu *WarpWindow::makeMenu(QWidget *parent, const QString &id, const QList<int> &lengths, bool showSave)
{
	auto *menu = new QMenu(parent);

	if (showSave) {
		menu->addAction(warp_flow_text("Warp.Window.Menu.Save"), this,
				[this, id]() { later([this, id]() { saveReplay(id, WARP_FLOW_LENGTH_DEFAULT); }); });
	}

	for (int seconds : lengths) {
		menu->addAction(warp_flow_text("Warp.Window.Menu.SaveLength").arg(warp_length_label(seconds)), this,
				[this, id, seconds]() { later([this, id, seconds]() { saveReplay(id, seconds); }); });
	}

	menu->addAction(warp_flow_text("Warp.Window.Menu.AddLast"), this,
			[this, id]() { later([this, id]() { addLastClip(id); }); });
	menu->addSeparator();
	menu->addAction(warp_flow_text("Warp.Window.Props"), this,
			[this, id]() { later([this, id]() { editFlow(id); }); });
	menu->addAction(warp_flow_text("Warp.Window.Remove"), this,
			[this, id]() { later([this, id]() { removeFlow(id); }); });

	return menu;
}

QWidget *WarpWindow::makeActions(const QString &id, const QString &name, const QList<int> &lengths)
{
	auto *holder = new QWidget(tree);
	auto *save = new QToolButton(holder);
	auto *more = new QToolButton(holder);
	auto *row = new QHBoxLayout(holder);

	row->setContentsMargins(4, 0, 6, 0);
	row->setSpacing(4);
	row->addWidget(save);
	row->addWidget(more);

	save->setText(warp_flow_text("Warp.Window.Save"));
	save->setToolTip(warp_flow_text("Warp.Window.Save.Tip").arg(name));
	save->setToolButtonStyle(Qt::ToolButtonTextOnly);

	more->setText(QStringLiteral("..."));
	more->setToolTip(warp_flow_text("Warp.Window.More"));
	more->setAccessibleName(warp_flow_text("Warp.Window.More"));
	more->setPopupMode(QToolButton::InstantPopup);
	/* the dots already say there is a menu behind it */
	more->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
	more->setMenu(makeMenu(more, id, lengths, false));

	connect(save, &QToolButton::clicked, this,
		[this, id]() { later([this, id]() { saveReplay(id, WARP_FLOW_LENGTH_DEFAULT); }); });

	saveButtons.insert(id, save);

	return holder;
}

QTreeWidgetItem *WarpWindow::addRow(QTreeWidgetItem *under, obs_data_t *flow, const QMap<QString, QString> &names)
{
	const QString id = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID));
	const QString name = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME));
	const char *kind = obs_data_get_string(flow, WARP_FLOW_KIND);
	const bool enabled = obs_data_get_bool(flow, WARP_FLOW_ENABLED);
	const bool instant = warp_is_instant(kind);

	auto *item = under ? new QTreeWidgetItem(under) : new QTreeWidgetItem(tree);

	QStringList links;

	for (const QString &link : warp_links(flow))
		links.append(names.value(link, link));

	const QStringList chips = warp_chips(flow);

	item->setText(WARP_COL_FLOW, name);
	item->setIcon(WARP_COL_FLOW, warp_flow_icon(kind));
	item->setData(WARP_COL_FLOW, WARP_ROLE_ID, id);
	item->setData(WARP_COL_FLOW, WARP_ROLE_KIND, QString::fromUtf8(kind));
	item->setData(WARP_COL_FLOW, WARP_ROLE_ENABLED, enabled);
	item->setData(WARP_COL_FLOW, WARP_ROLE_CHIPS, chips);
	item->setData(WARP_COL_FLOW, WARP_ROLE_LINKS, links);
	item->setToolTip(WARP_COL_FLOW,
			 warp_flow_kind_name(kind) + QStringLiteral("\n") + chips.join(QStringLiteral(" - ")));

	item->setText(WARP_COL_FEEDS, QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_TARGET_NAME)));
	item->setData(WARP_COL_FEEDS, WARP_ROLE_SUBTITLE,
		      instant ? warp_flow_text("Warp.Window.Feeds.Media")
			      : warp_flow_text("Warp.Window.Feeds.Playlist"));

	item->setData(WARP_COL_CLIPS, WARP_ROLE_INSTANT, instant);
	item->setData(WARP_COL_CLIPS, WARP_ROLE_MAX_CLIPS,
		      instant ? 0 : (int)obs_data_get_int(flow, WARP_FLOW_MAX_CLIPS));

	tree->setItemWidget(item, WARP_COL_ON, makeSwitch(id, name, enabled));
	tree->setItemWidget(item, WARP_COL_ACTIONS, makeActions(id, name, warp_other_lengths(flow)));

	return item;
}

void WarpWindow::refresh()
{
	const QString selected = selectedId();
	const int scrolled = tree->verticalScrollBar() ? tree->verticalScrollBar()->value() : 0;

	saveButtons.clear();
	fedBy.clear();
	tree->clear();

	obs_data_array_t *flows = warp_flow_list();
	const size_t count = obs_data_array_count(flows);

	QList<obs_data_t *> list;
	QMap<QString, int> index;
	QMap<QString, QString> names;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *flow = obs_data_array_item(flows, i);
		const QString id = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID));

		index.insert(id, list.size());
		names.insert(id, QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME)));
		list.append(flow);
	}

	/* Each flow is set in under the first flow that feeds it. One fed by
	 * several shows under the first of them, and lists the rest in the
	 * panel; flows that only feed each other round in a loop are put at the
	 * top, where the first of them is reached. */
	QMap<QString, QString> parent;
	int taking = 0;

	for (obs_data_t *flow : list) {
		const QString id = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID));

		if (obs_data_get_bool(flow, WARP_FLOW_ENABLED))
			taking++;

		for (const QString &link : warp_links(flow)) {
			if (link == id || !index.contains(link))
				continue;

			fedBy[link].append(names.value(id));

			if (!parent.contains(link))
				parent.insert(link, id);
		}
	}

	QSet<QString> placed;
	std::function<void(int, QTreeWidgetItem *)> place = [&](int i, QTreeWidgetItem *under) {
		obs_data_t *flow = list[i];
		const QString id = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID));

		if (placed.contains(id))
			return;

		placed.insert(id);

		QTreeWidgetItem *item = addRow(under, flow, names);

		for (const QString &link : warp_links(flow)) {
			if (parent.value(link) == id && index.contains(link))
				place(index.value(link), item);
		}
	};

	for (int i = 0; i < list.size(); i++) {
		if (!parent.contains(QString::fromUtf8(obs_data_get_string(list[i], WARP_FLOW_ID))))
			place(i, nullptr);
	}

	for (int i = 0; i < list.size(); i++)
		place(i, nullptr);

	for (obs_data_t *flow : list)
		obs_data_release(flow);

	obs_data_array_release(flows);

	tree->expandAll();

	countLabel->setText(warp_flow_text("Warp.Window.Count").arg(taking).arg(list.size()));
	countLabel->setVisible(!list.isEmpty());
	listStack->setCurrentIndex(list.isEmpty() ? 1 : 0);
	/* with nothing to pick, there is nothing for the panel to say */
	inspector->setVisible(!list.isEmpty());

	QTreeWidgetItem *item = itemFor(selected);

	if (!item && tree->topLevelItemCount() > 0)
		item = tree->topLevelItem(0);

	if (item)
		tree->setCurrentItem(item);

	if (tree->verticalScrollBar())
		tree->verticalScrollBar()->setValue(scrolled);

	buildInspector();
	refreshStatus();
}

void WarpWindow::refreshStatus()
{
	const QMap<obs_hotkey_id, QStringList> keys = warp_bound_keys();

	for (QTreeWidgetItemIterator it(tree); *it; ++it) {
		QTreeWidgetItem *item = *it;
		const QString id = item->data(WARP_COL_FLOW, WARP_ROLE_ID).toString();
		const QByteArray utf8 = id.toUtf8();
		const int clips = warp_flow_clip_count(utf8.constData());
		const bool missing = clips < 0;

		if (item->data(WARP_COL_CLIPS, WARP_ROLE_CLIPS).toInt() != clips)
			item->setData(WARP_COL_CLIPS, WARP_ROLE_CLIPS, clips);
		if (item->data(WARP_COL_CLIPS, WARP_ROLE_MISSING).toBool() != missing) {
			item->setData(WARP_COL_CLIPS, WARP_ROLE_MISSING, missing);
			item->setData(WARP_COL_FEEDS, WARP_ROLE_MISSING, missing);
		}

		const int max = item->data(WARP_COL_CLIPS, WARP_ROLE_MAX_CLIPS).toInt();

		item->setToolTip(WARP_COL_CLIPS,
				 max > 0 && clips >= max ? warp_flow_text("Warp.Window.Clips.Full") : QString());

		/* the Save button wears the key it shares a job with, so an
		 * operator learns the key from the button */
		QToolButton *save = saveButtons.value(id);

		if (!save)
			continue;

		struct warp_flow_hotkeys hotkeys;
		QString key;

		if (warp_flow_get_hotkeys(utf8.constData(), &hotkeys))
			key = warp_key_text(keys, hotkeys.save);

		const QString text = key.isEmpty() ? warp_flow_text("Warp.Window.Save")
						   : warp_flow_text("Warp.Window.Save.Key").arg(key);

		if (save->text() != text)
			save->setText(text);

		const QString name = item->text(WARP_COL_FLOW);

		save->setEnabled(!missing);
		save->setToolTip(missing ? warp_flow_text("Warp.Window.Save.Missing").arg(name)
					 : warp_flow_text("Warp.Window.Save.Tip").arg(name));
	}

	/* the panel says what can be done about a source that has gone, so it
	 * is built again when one goes or comes back */
	const QString selected = selectedId();

	if (!selected.isEmpty()) {
		const int missing = warp_flow_clip_count(selected.toUtf8().constData()) < 0 ? 1 : 0;

		if (inspectorMissing != -1 && missing != inspectorMissing)
			buildInspector();
	}

	refreshBuffer();
	refreshFooter();
}

void WarpWindow::refreshBuffer()
{
	const bool active = warp_flow_replay_buffer_active();

	/* the banner is only worth its room while there is a flow it is
	 * holding up */
	banner->setVisible(!active && tree->topLevelItemCount() > 0);

	if (bufferShown == (int)active)
		return;

	bufferShown = (int)active;

	const int size = bufferDot->fontMetrics().height() * 3 / 4;

	bufferDot->setPixmap(warp_buffer_icon(active).pixmap(QSize(size, size), bufferDot->devicePixelRatioF()));
	bufferLabel->setText(active ? warp_flow_text("Warp.Window.Buffer.Running")
				    : warp_flow_text("Warp.Window.Buffer.Stopped"));
	bufferButton->setText(active ? warp_flow_text("Warp.Window.Buffer.Stop")
				     : warp_flow_text("Warp.Window.Buffer.Start"));
}

void WarpWindow::refreshFooter()
{
	const QString label =
		QStringLiteral("<b>%1</b>&nbsp;&nbsp;").arg(warp_flow_text("Warp.Window.Footer.Label").toHtmlEscaped());
	char *last = warp_flow_last_clip();
	obs_data_t *landed = warp_flow_last_landed();
	const QString lastPath = last ? QString::fromUtf8(last) : QString();

	bfree(last);

	if (lastPath.isEmpty() && !landed) {
		footer->setText(label + warp_flow_text("Warp.Window.Footer.None").toHtmlEscaped());
		return;
	}

	const QString landedPath = landed ? QString::fromUtf8(obs_data_get_string(landed, WARP_FLOW_LANDED_PATH))
					  : QString();
	const QString path = lastPath.isEmpty() ? landedPath : lastPath;
	const QString dim = warp_color_css(warp_dim_color());
	QString text = label + QFileInfo(path).fileName().toHtmlEscaped();

	if (landed && landedPath == path) {
		text += QStringLiteral(" <span style=\"color: %1\">&middot; %2</span>")
				.arg(dim, warp_ago((uint64_t)obs_data_get_int(landed, WARP_FLOW_LANDED_TIME))
						  .toHtmlEscaped());

		obs_data_array_t *flows = obs_data_get_array(landed, WARP_FLOW_LANDED_FLOWS);
		const size_t count = flows ? obs_data_array_count(flows) : 0;
		QStringList reached;

		for (size_t i = 0; i < count; i++) {
			obs_data_t *flow = obs_data_array_item(flows, i);
			const QString name = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_LANDED_NAME));

			if (obs_data_get_bool(flow, WARP_FLOW_LANDED_OK))
				reached.append(name.toHtmlEscaped());
			else
				reached.append(QStringLiteral("<span style=\"color: %1\"><b>%2</b></span>")
						       .arg(warp_color_css(warp_red),
							    warp_flow_text("Warp.Window.Footer.Failed")
								    .arg(name)
								    .toHtmlEscaped()));

			obs_data_release(flow);
		}

		obs_data_array_release(flows);

		text += QStringLiteral(" &nbsp;&rarr;&nbsp; ");
		text += reached.isEmpty()
				? QStringLiteral("<span style=\"color: %1\">%2</span>")
					  .arg(dim, warp_flow_text("Warp.Window.Footer.Nobody").toHtmlEscaped())
				: reached.join(QStringLiteral(", "));
	} else if (landed) {
		/* saved, and still being cut for the flows it is going to */
		text += QStringLiteral(" <span style=\"color: %1\">&middot; %2</span>")
				.arg(dim, warp_flow_text("Warp.Window.Footer.OnItsWay").toHtmlEscaped());
	}

	obs_data_release(landed);

	if (footer->text() != text)
		footer->setText(text);
}

void WarpWindow::buildInspector()
{
	const QString id = selectedId();
	auto *panel = new QWidget();
	auto *layout = new QVBoxLayout(panel);

	layout->setContentsMargins(14, 6, 6, 6);
	layout->setSpacing(8);

	obs_data_t *flow = id.isEmpty() ? nullptr : warp_flow_get(id.toUtf8().constData());

	if (!flow) {
		inspectorMissing = -1;

		auto *hint = new QLabel(warp_flow_text("Warp.Window.Inspector.Empty"), panel);

		hint->setWordWrap(true);
		hint->setAlignment(Qt::AlignCenter);
		hint->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_dim_color())));
		layout->addStretch(1);
		layout->addWidget(hint);
		layout->addStretch(2);
		inspector->setWidget(panel);
		return;
	}

	const QByteArray utf8 = id.toUtf8();
	const QString name = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME));
	const char *kind = obs_data_get_string(flow, WARP_FLOW_KIND);
	const bool instant = warp_is_instant(kind);
	const bool enabled = obs_data_get_bool(flow, WARP_FLOW_ENABLED);
	const bool missing = warp_flow_clip_count(utf8.constData()) < 0;
	const QString target = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_TARGET_NAME));
	const QList<int> lengths = warp_other_lengths(flow);

	inspectorMissing = missing ? 1 : 0;

	/* which flow, and what kind */
	auto *icon = new QLabel(panel);

	icon->setPixmap(warp_flow_icon(kind).pixmap(QSize(28, 28), devicePixelRatioF()));

	auto *nameLabel = new QLabel(name, panel);
	QFont nameFont = nameLabel->font();

	nameFont.setBold(true);

	if (nameFont.pointSizeF() > 0)
		nameFont.setPointSizeF(nameFont.pointSizeF() * 1.15);

	nameLabel->setFont(nameFont);
	nameLabel->setTextFormat(Qt::PlainText);
	nameLabel->setWordWrap(true);

	const QString kindName = warp_flow_kind_name(kind);
	auto *kindLabel =
		new QLabel(enabled ? kindName : warp_flow_text("Warp.Window.Inspector.Off").arg(kindName), panel);

	kindLabel->setWordWrap(true);
	kindLabel->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_dim_color())));

	auto *names = new QVBoxLayout();

	names->setSpacing(1);
	names->addWidget(nameLabel);
	names->addWidget(kindLabel);

	auto *titleRow = new QHBoxLayout();

	titleRow->setSpacing(10);
	titleRow->addWidget(icon, 0, Qt::AlignTop);
	titleRow->addLayout(names, 1);
	layout->addLayout(titleRow);

	/* what can be done with it from here */
	auto *actions = new QHBoxLayout();

	actions->setSpacing(6);

	if (missing) {
		auto *pick = new QPushButton(warp_flow_text("Warp.Window.Inspector.PickSource"), panel);

		connect(pick, &QPushButton::clicked, this, [this, id]() { later([this, id]() { editFlow(id); }); });
		actions->addWidget(pick);
	} else {
		auto *save = new QPushButton(warp_flow_text("Warp.Window.Inspector.Save"), panel);
		auto *addLast = new QPushButton(warp_flow_text("Warp.Window.Inspector.AddLast"), panel);

		connect(save, &QPushButton::clicked, this,
			[this, id]() { later([this, id]() { saveReplay(id, WARP_FLOW_LENGTH_DEFAULT); }); });
		connect(addLast, &QPushButton::clicked, this,
			[this, id]() { later([this, id]() { addLastClip(id); }); });
		actions->addWidget(save);
		actions->addWidget(addLast);
	}

	actions->addStretch(1);
	layout->addLayout(actions);

	/* setting it up, and the one thing that cannot be taken back, a row of
	 * their own and kept to opposite ends of it */
	auto *props = new QPushButton(warp_flow_text("Warp.Window.Props"), panel);
	auto *remove = new QPushButton(warp_flow_text("Warp.Window.Remove"), panel);

	remove->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_red)));

	connect(props, &QPushButton::clicked, this, [this, id]() { later([this, id]() { editFlow(id); }); });
	connect(remove, &QPushButton::clicked, this, [this, id]() { later([this, id]() { removeFlow(id); }); });

	auto *manage = new QHBoxLayout();

	manage->setSpacing(6);
	manage->addWidget(props);
	manage->addStretch(1);
	manage->addWidget(remove);
	layout->addLayout(manage);
	layout->addWidget(warp_rule(panel));

	/* everything about it, in full */
	layout->addWidget(warp_section_label(warp_flow_text("Warp.Window.Inspector.Settings"), panel));

	auto *settings = new QFormLayout();

	settings->setHorizontalSpacing(12);
	settings->setVerticalSpacing(6);
	settings->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
	settings->setRowWrapPolicy(QFormLayout::DontWrapRows);
	settings->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	auto row = [&](const QString &label, QLabel *value) {
		auto *key = new QLabel(label, panel);

		key->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_dim_color())));
		settings->addRow(key, value);
	};

	QLabel *feeds =
		warp_value_label(missing ? warp_flow_text("Warp.Window.Inspector.Missing").arg(target) : target, panel);

	if (missing)
		feeds->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(warp_color_css(warp_red)));

	row(warp_flow_text("Warp.Flow.Target"), feeds);
	row(warp_flow_text("Warp.Flow.Trigger"),
	    warp_value_label(warp_trigger_name(obs_data_get_string(flow, WARP_FLOW_TRIGGER)), panel));

	if (instant) {
		const int speed = (int)obs_data_get_int(flow, WARP_FLOW_SPEED);

		row(warp_flow_text("Warp.Flow.Playback"),
		    warp_value_label(warp_playback_name(obs_data_get_string(flow, WARP_FLOW_PLAYBACK)), panel));
		row(warp_flow_text("Warp.Flow.Speed.Label"),
		    warp_value_label(speed > 0 ? warp_flow_text("Warp.Window.Inspector.Speed").arg(speed)
					       : warp_flow_text("Warp.Window.Inspector.OwnSpeed"),
				     panel));
	} else {
		const char *order = obs_data_get_string(flow, WARP_FLOW_ORDER);
		const int max = (int)obs_data_get_int(flow, WARP_FLOW_MAX_CLIPS);

		row(warp_flow_text("Warp.Flow.Order"),
		    warp_value_label(order && strcmp(order, WARP_FLOW_ORDER_NEWEST_FIRST) == 0
					     ? warp_flow_text("Warp.Flow.Order.NewestFirst")
					     : warp_flow_text("Warp.Flow.Order.OldestFirst"),
				     panel));
		row(warp_flow_text("Warp.Flow.Limit.Label"),
		    warp_value_label(max > 0 ? warp_flow_text("Warp.Window.Inspector.Limit").arg(max)
					     : warp_flow_text("Warp.Window.Inspector.NoLimit"),
				     panel));
	}

	const int length = (int)obs_data_get_int(flow, WARP_FLOW_LENGTH);

	row(warp_flow_text("Warp.Flow.Length.Label"),
	    warp_value_label(length > 0 ? warp_flow_text("Warp.Window.Inspector.Seconds").arg(length)
					: warp_flow_text("Warp.Flow.Length.Whole"),
			     panel));

	if (!lengths.isEmpty()) {
		QStringList others;

		for (int seconds : lengths)
			others.append(warp_length_label(seconds));

		row(warp_flow_text("Warp.Flow.Lengths.Label"),
		    warp_value_label(others.join(QStringLiteral(", ")), panel));
	}

	QStringList links;

	for (const QString &link : warp_links(flow)) {
		obs_data_t *linked = warp_flow_get(link.toUtf8().constData());

		links.append(linked ? QString::fromUtf8(obs_data_get_string(linked, WARP_FLOW_NAME)) : link);
		obs_data_release(linked);
	}

	if (!links.isEmpty())
		row(warp_flow_text("Warp.Window.Inspector.Links"),
		    warp_value_label(links.join(QStringLiteral(", ")), panel));

	if (fedBy.contains(id))
		row(warp_flow_text("Warp.Flow.FedBy"),
		    warp_value_label(fedBy.value(id).join(QStringLiteral(", ")), panel));

	row(warp_flow_text("Warp.Window.Inspector.Status"),
	    warp_value_label(enabled ? warp_flow_text("Warp.Window.Inspector.On")
				     : warp_flow_text("Warp.Window.Inspector.OffValue"),
			     panel));

	layout->addLayout(settings);
	layout->addWidget(warp_rule(panel));

	/* the keys that run it, which are how it is run most of the time */
	layout->addWidget(warp_section_label(warp_flow_text("Warp.Window.Inspector.Hotkeys"), panel));

	auto *where = new QLabel(warp_flow_text("Warp.Window.Inspector.HotkeysWhere"), panel);

	where->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_dim_color())));
	layout->addWidget(where);

	auto *hotkeyForm = new QFormLayout();

	hotkeyForm->setHorizontalSpacing(12);
	hotkeyForm->setVerticalSpacing(6);
	hotkeyForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	const QMap<obs_hotkey_id, QStringList> keys = warp_bound_keys();
	struct warp_flow_hotkeys hotkeys;

	warp_flow_get_hotkeys(utf8.constData(), &hotkeys);

	auto keyRow = [&](const QString &label, obs_hotkey_id hotkey) {
		const QString key = warp_key_text(keys, hotkey);
		auto *value = new QLabel(key.isEmpty() ? warp_flow_text("Warp.Window.Hotkey.Unset") : key, panel);

		value->setTextFormat(Qt::PlainText);
		value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		if (key.isEmpty()) {
			value->setStyleSheet(QStringLiteral("color: %1;").arg(warp_color_css(warp_dim_color())));
		} else {
			QFont bold = value->font();

			bold.setBold(true);
			value->setFont(bold);
		}

		hotkeyForm->addRow(new QLabel(label, panel), value);
	};

	keyRow(warp_flow_text("Warp.Window.Menu.Save"), hotkeys.save);
	keyRow(warp_flow_text("Warp.Window.Menu.AddLast"), hotkeys.promote);

	for (size_t i = 0; i < hotkeys.lengths_num; i++)
		keyRow(warp_flow_text("Warp.Window.Menu.SaveLength").arg(warp_length_label(hotkeys.length_seconds[i])),
		       hotkeys.length_hotkeys[i]);

	layout->addLayout(hotkeyForm);
	layout->addStretch(1);

	obs_data_release(flow);

	/* the panel that was there goes with it */
	inspector->setWidget(panel);
}

void WarpWindow::addFlow()
{
	WarpFlowCreateDialog dialog(this);

	if (dialog.exec() != QDialog::Accepted)
		return;

	refresh();

	const QStringList made = dialog.createdIds();

	if (!made.isEmpty())
		selectId(made.last());
}

void WarpWindow::editFlow(const QString &id)
{
	if (id.isEmpty())
		return;

	obs_data_t *flow = warp_flow_get(id.toUtf8().constData());

	if (!flow)
		return;

	obs_data_release(flow);

	WarpFlowPropsDialog dialog(this, id);

	if (dialog.exec() == QDialog::Accepted)
		refresh();
}

void WarpWindow::removeFlow(const QString &id)
{
	obs_data_t *flow = id.isEmpty() ? nullptr : warp_flow_get(id.toUtf8().constData());

	if (!flow)
		return;

	const QString name = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME));

	obs_data_release(flow);

	const QMessageBox::StandardButton answer =
		QMessageBox::question(this, warp_flow_text("Warp.Window.Remove.Title"),
				      warp_flow_text("Warp.Window.Remove.Confirm").arg(name),
				      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

	if (answer != QMessageBox::Yes)
		return;

	warp_flow_remove(id.toUtf8().constData());
	refresh();
}

void WarpWindow::setFlowEnabled(const QString &id, bool enabled)
{
	obs_data_t *update = obs_data_create();

	obs_data_set_bool(update, WARP_FLOW_ENABLED, enabled);
	warp_flow_update(id.toUtf8().constData(), update);
	obs_data_release(update);

	refresh();
}

void WarpWindow::saveReplay(const QString &id, int seconds)
{
	const QByteArray utf8 = id.toUtf8();

	/* pressed with nothing to save, it asks the way the flow's own hotkey
	 * does, and offers to start the buffer there and then */
	if (!warp_flow_replay_buffer_active()) {
		obs_data_t *flow = warp_flow_get(utf8.constData());

		if (flow) {
			warp_window_buffer_prompt(obs_data_get_string(flow, WARP_FLOW_NAME));
			obs_data_release(flow);
		}

		return;
	}

	warp_flow_save_replay_length(utf8.constData(), seconds);
	refreshStatus();
}

void WarpWindow::addLastClip(const QString &id)
{
	warp_flow_promote_last(id.toUtf8().constData());
	refreshStatus();
}

void WarpWindow::toggleBuffer()
{
	if (!obs_frontend_replay_buffer_active()) {
		obs_frontend_replay_buffer_start();
		return;
	}

	/* stopping it throws away what it is holding, and stops every flow,
	 * so it is asked about rather than done on a stray click */
	const QMessageBox::StandardButton answer = QMessageBox::question(
		this, warp_flow_text("Warp.Window.Buffer.StopTitle"), warp_flow_text("Warp.Window.Buffer.StopConfirm"),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

	if (answer == QMessageBox::Yes && obs_frontend_replay_buffer_active())
		obs_frontend_replay_buffer_stop();
}

void WarpWindow::showHelp()
{
	QMessageBox box(this);

	box.setWindowTitle(warp_flow_text("Warp.Window.Help"));
	box.setIcon(QMessageBox::Information);
	box.setText(warp_flow_text("Warp.Window.Intro"));
	box.exec();
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

/* The main window, whose status bar Warp's messages go in. Held on to rather
 * than asked for each time: the frontend's pointer outlives the window on the
 * way out, and this one knows when it has gone. */
QPointer<QMainWindow> warp_status_window;

/* On the UI thread, with a message to put up. OBS's own status bar is a
 * QStatusBar underneath, so a message is shown the way OBS shows its own: on
 * the left, over whatever OBS said last, until it times out or something else
 * is said. */
void warp_window_show_status(const char *message, int timeout_ms)
{
	if (!warp_status_window || !message)
		return;

	QStatusBar *bar = warp_status_window->statusBar();

	if (bar)
		bar->showMessage(QString::fromUtf8(message), timeout_ms);
}

void warp_status_frontend_event(enum obs_frontend_event event, void *)
{
	/* nothing is said on the way out: there is nobody left to read it */
	if (event == OBS_FRONTEND_EVENT_EXIT)
		warp_status_set_sink(nullptr);
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

	warp_status_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	warp_status_set_sink(warp_window_show_status);
	obs_frontend_add_event_callback(warp_status_frontend_event, nullptr);
}
