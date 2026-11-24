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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "warp-events.h"
#include "warp-flow-dialog.hpp"
#include "warp-flow.h"

namespace {

constexpr double WARP_PI = 3.14159265358979323846;

/* the kinds the create dialog offers, in the order they are listed */
struct WarpFlowKind {
	const char *id;
	const char *name_key;
	const char *desc_key;
};

const WarpFlowKind warp_flow_kinds[] = {
	{WARP_FLOW_KIND_REPLAY, "Warp.Flow.Kind.Replay", "Warp.Flow.Kind.Replay.Desc"},
	{WARP_FLOW_KIND_HIGHLIGHT, "Warp.Flow.Kind.Highlight", "Warp.Flow.Kind.Highlight.Desc"},
	{WARP_FLOW_KIND_COMBO, "Warp.Flow.Kind.Combo", "Warp.Flow.Kind.Combo.Desc"},
	{WARP_FLOW_KIND_INSTANT, "Warp.Flow.Kind.Instant", "Warp.Flow.Kind.Instant.Desc"},
};

QPolygonF warp_star(const QPointF &center, qreal outer, qreal inner)
{
	QPolygonF poly;

	for (int i = 0; i < 10; i++) {
		const qreal radius = (i % 2 == 0) ? outer : inner;
		const qreal angle = -WARP_PI / 2.0 + i * WARP_PI / 5.0;

		poly << QPointF(center.x() + radius * std::cos(angle), center.y() + radius * std::sin(angle));
	}

	return poly;
}

void warp_draw_play(QPainter &p, const QRectF &box, const QColor &color)
{
	QPolygonF triangle;

	triangle << QPointF(box.left(), box.top()) << QPointF(box.right(), box.center().y())
		 << QPointF(box.left(), box.bottom());

	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawPolygon(triangle);
}

/* the Warp source a flow of that kind feeds: an instant replay holds its one
 * clip in a Warp Media source, every other kind builds a list in a Warp
 * Playlist source */
const char *warp_flow_target_id(const QString &kind)
{
	return kind == QString(WARP_FLOW_KIND_INSTANT) ? WARP_MEDIA_SOURCE_ID : WARP_PLAYLIST_SOURCE_ID;
}

bool warp_flow_target_is_media(const char *source_id)
{
	return strcmp(source_id, WARP_MEDIA_SOURCE_ID) == 0;
}

struct WarpTargetSearch {
	const char *id;
	QList<QPair<QString, QString>> found;
};

bool warp_enum_target_sources(void *param, obs_source_t *source)
{
	auto *search = static_cast<WarpTargetSearch *>(param);
	const char *id = obs_source_get_id(source);

	if (id && strcmp(id, search->id) == 0) {
		const char *name = obs_source_get_name(source);
		const char *uuid = obs_source_get_uuid(source);

		search->found.append({QString::fromUtf8(name ? name : ""), QString::fromUtf8(uuid ? uuid : "")});
	}

	return true;
}

/* Every source of that kind in the scene collection, with an entry for making
 * one on the end of it. The entry that is picked carries the source's uuid, or
 * an empty string for the new one. */
void warp_fill_target_combo(QComboBox *combo, const QString &selected_uuid, const char *source_id)
{
	WarpTargetSearch search = {source_id, {}};

	obs_enum_sources(warp_enum_target_sources, &search);
	std::sort(search.found.begin(), search.found.end(),
		  [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
			  return a.first.localeAwareCompare(b.first) < 0;
		  });

	combo->clear();

	for (const auto &source : search.found)
		combo->addItem(source.first, source.second);

	combo->addItem(warp_flow_text(warp_flow_target_is_media(source_id) ? "Warp.Flow.Target.New.Media"
									   : "Warp.Flow.Target.New"),
		       QString());

	int index = selected_uuid.isEmpty() ? -1 : combo->findData(selected_uuid);

	/* a source to feed has to be made when there is not one yet */
	if (index < 0)
		index = search.found.isEmpty() ? combo->count() - 1 : 0;

	combo->setCurrentIndex(index);
}

/* A Warp source called 'wanted', or as close to it as a name that is not taken
 * already gets. It is put in the current scene: a source no scene holds on to
 * is not saved with the scene collection. */
obs_source_t *warp_create_target_source(const QString &wanted, const char *source_id)
{
	QString name = wanted;

	for (int suffix = 2; suffix < 1000; suffix++) {
		obs_source_t *taken = obs_get_source_by_name(name.toUtf8().constData());

		if (!taken)
			break;

		obs_source_release(taken);
		name = QString("%1 %2").arg(wanted).arg(suffix);
	}

	obs_data_t *settings = obs_data_create();
	obs_source_t *source = obs_source_create(source_id, name.toUtf8().constData(), settings, nullptr);

	obs_data_release(settings);

	if (!source)
		return nullptr;

	obs_source_t *scene_source = obs_frontend_get_current_scene();
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;

	if (scene)
		obs_scene_add(scene, source);
	else
		blog(LOG_WARNING, "[Warp Flow]: no scene to put '%s' in", name.toUtf8().constData());

	obs_source_release(scene_source);

	return source;
}

/* The source the flow is to feed, made first when that is what was asked for.
 * Answers false, having said why, when there is nothing to feed. */
bool warp_resolve_target(QWidget *parent, QComboBox *combo, QLineEdit *new_name, const QString &fallback_name,
			 const char *source_id, QString &uuid, QString &name)
{
	const QString chosen = combo->currentData().toString();

	if (!chosen.isEmpty()) {
		obs_source_t *source = obs_get_source_by_uuid(chosen.toUtf8().constData());

		if (!source) {
			QMessageBox::warning(parent, warp_flow_text("Warp.Flow.Create.Title"),
					     warp_flow_text("Warp.Flow.Error.TargetGone"));
			return false;
		}

		const char *source_name = obs_source_get_name(source);

		uuid = chosen;
		name = QString::fromUtf8(source_name ? source_name : "");
		obs_source_release(source);

		return true;
	}

	QString wanted = new_name ? new_name->text().trimmed() : QString();

	if (wanted.isEmpty())
		wanted = fallback_name;

	obs_source_t *source = warp_create_target_source(wanted, source_id);

	if (!source) {
		QMessageBox::warning(parent, warp_flow_text("Warp.Flow.Create.Title"),
				     warp_flow_text("Warp.Flow.Error.TargetNotMade"));
		return false;
	}

	const char *made_name = obs_source_get_name(source);
	const char *made_uuid = obs_source_get_uuid(source);

	uuid = QString::fromUtf8(made_uuid ? made_uuid : "");
	name = QString::fromUtf8(made_name ? made_name : "");
	obs_source_release(source);

	return true;
}

/* a name no other flow is using already */
bool warp_flow_name_free(QWidget *parent, const QString &name, const QString &except_id)
{
	obs_data_t *existing = warp_flow_get_by_name(name.toUtf8().constData());

	if (!existing)
		return true;

	const bool same = except_id == QString::fromUtf8(obs_data_get_string(existing, WARP_FLOW_ID));

	obs_data_release(existing);

	if (same)
		return true;

	QMessageBox::warning(parent, warp_flow_text("Warp.Flow.Create.Title"),
			     warp_flow_text("Warp.Flow.Error.NameTaken"));

	return false;
}

void warp_set_lengths(obs_data_t *config, const QList<int> &lengths)
{
	obs_data_array_t *array = obs_data_array_create();

	for (int seconds : lengths) {
		obs_data_t *item = obs_data_create();

		obs_data_set_int(item, WARP_FLOW_LENGTH_SECONDS, seconds);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	obs_data_set_array(config, WARP_FLOW_LENGTHS, array);
	obs_data_array_release(array);
}

/* A row of buttons only one of which is down at a time, each standing for a
 * value: what a pair of choices is put as, rather than a combo box that hides
 * the other one. */
class WarpSegment : public QWidget {
public:
	std::function<void()> changed;

	explicit WarpSegment(QWidget *parent) : QWidget(parent)
	{
		row = new QHBoxLayout(this);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(0);
		group = new QButtonGroup(this);
		connect(group, &QButtonGroup::idClicked, this, [this](int) {
			if (changed)
				changed();
		});
	}

	void add(const QString &text, const QString &value, const QString &tip = QString())
	{
		auto *button = new QPushButton(text, this);

		button->setCheckable(true);
		button->setToolTip(tip);
		button->setChecked(values.isEmpty());
		group->addButton(button, (int)values.size());
		values.append(value);
		row->addWidget(button);
	}

	QString value() const
	{
		int id = group->checkedId();
		return id >= 0 ? values[id] : QString();
	}

	void setValue(const QString &value)
	{
		int index = (int)values.indexOf(value);

		if (index >= 0)
			group->button(index)->setChecked(true);
	}

private:
	QHBoxLayout *row = nullptr;
	QButtonGroup *group = nullptr;
	QStringList values;
};

/* the heading of a section, set small and let back */
QLabel *warp_section_title(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text.toUpper(), parent);
	QFont font = label->font();

	font.setBold(true);
	font.setPointSizeF(font.pointSizeF() * 0.85);
	label->setFont(font);
	label->setEnabled(false);
	label->setMinimumWidth(110);
	label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	return label;
}

/* a field with what it is above it */
QWidget *warp_labelled(QLabel *label, QWidget *field, QWidget *parent)
{
	auto *box = new QWidget(parent);
	auto *column = new QVBoxLayout(box);

	column->setContentsMargins(0, 0, 0, 0);
	column->setSpacing(4);
	column->addWidget(label);
	column->addWidget(field);
	return box;
}

QWidget *warp_hbox(QWidget *parent, std::initializer_list<QWidget *> widgets)
{
	auto *box = new QWidget(parent);
	auto *row = new QHBoxLayout(box);

	row->setContentsMargins(0, 0, 0, 0);
	for (QWidget *widget : widgets)
		row->addWidget(widget);
	row->addStretch(1);
	return box;
}

} // namespace

/* ------------------------------------------------------------------------- */

QString warp_flow_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QIcon warp_flow_icon(const char *kind)
{
	const int size = 64;
	QPixmap pixmap(size, size);

	pixmap.fill(Qt::transparent);

	QPainter p(&pixmap);

	p.setRenderHint(QPainter::Antialiasing, true);

	const QColor fg = QApplication::palette().color(QPalette::WindowText);
	QPen pen(fg);

	pen.setWidthF(size / 16.0);
	pen.setJoinStyle(Qt::RoundJoin);

	const bool combo = kind && strcmp(kind, WARP_FLOW_KIND_COMBO) == 0;
	const bool highlight = kind && strcmp(kind, WARP_FLOW_KIND_HIGHLIGHT) == 0;
	const bool instant = kind && strcmp(kind, WARP_FLOW_KIND_INSTANT) == 0;

	if (instant) {
		/* one screen, with the bolt of a clip that is there the moment it
		 * is saved rather than the play mark of a list that is worked
		 * through */
		QPolygonF bolt;

		bolt << QPointF(size * 0.56, size * 0.30) << QPointF(size * 0.38, size * 0.52)
		     << QPointF(size * 0.49, size * 0.52) << QPointF(size * 0.44, size * 0.70)
		     << QPointF(size * 0.62, size * 0.48) << QPointF(size * 0.51, size * 0.48);

		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(size * 0.12, size * 0.18, size * 0.76, size * 0.64), size * 0.12, size * 0.12);

		p.setPen(Qt::NoPen);
		p.setBrush(fg);
		p.drawPolygon(bolt);
	} else if (highlight) {
		p.setPen(Qt::NoPen);
		p.setBrush(fg);
		p.drawPolygon(warp_star(QPointF(size / 2.0, size / 2.0), size * 0.42, size * 0.18));
	} else if (combo) {
		/* a list behind a list, with the star of the highlight one
		 * sitting on the front of them */
		const QRectF back(size * 0.22, size * 0.10, size * 0.60, size * 0.50);
		const QRectF front(size * 0.10, size * 0.32, size * 0.56, size * 0.46);

		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(back, size * 0.10, size * 0.10);

		/* the front list is drawn over the back one rather than
		 * through it, so what is behind is cleared out first */
		p.setCompositionMode(QPainter::CompositionMode_Clear);
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::black);
		p.drawRoundedRect(front.adjusted(-pen.widthF(), -pen.widthF(), pen.widthF(), pen.widthF()), size * 0.10,
				  size * 0.10);

		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(front, size * 0.10, size * 0.10);
		warp_draw_play(p, QRectF(size * 0.24, size * 0.42, size * 0.18, size * 0.26), fg);

		p.setPen(Qt::NoPen);
		p.setBrush(fg);
		p.drawPolygon(warp_star(QPointF(size * 0.78, size * 0.76), size * 0.21, size * 0.09));
	} else {
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(size * 0.12, size * 0.18, size * 0.76, size * 0.64), size * 0.12, size * 0.12);
		warp_draw_play(p, QRectF(size * 0.38, size * 0.36, size * 0.24, size * 0.28), fg);
	}

	p.end();

	return QIcon(pixmap);
}

QString warp_flow_kind_name(const char *kind)
{
	for (const WarpFlowKind &entry : warp_flow_kinds) {
		if (kind && strcmp(kind, entry.id) == 0)
			return warp_flow_text(entry.name_key);
	}

	return warp_flow_text(warp_flow_kinds[0].name_key);
}

QString warp_length_label(int seconds)
{
	char label[WARP_FLOW_LENGTH_LABEL_SIZE];

	warp_flow_length_label(seconds, label, sizeof(label));

	return QString::fromUtf8(label);
}

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

WarpSwitch::WarpSwitch(QWidget *parent) : QAbstractButton(parent)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::StrongFocus);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void WarpSwitch::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
	const qreal radius = track.height() / 2.0;
	QColor ink = palette().color(QPalette::WindowText);
	QColor faint = ink;

	p.setRenderHint(QPainter::Antialiasing, true);
	faint.setAlphaF(0.25);
	ink.setAlphaF(0.7);

	p.setPen(Qt::NoPen);
	p.setBrush(isChecked() ? QColor(0x3E, 0xC7, 0x6A) : faint);
	p.drawRoundedRect(track, radius, radius);

	const qreal knob = track.height() - 4.0;
	const qreal x = isChecked() ? track.right() - 2.0 - knob : track.left() + 2.0;

	p.setBrush(isChecked() ? QColor(Qt::white) : ink);
	p.drawEllipse(QRectF(x, track.top() + 2.0, knob, knob));

	if (hasFocus()) {
		p.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(track, radius, radius);
	}
}

/* ------------------------------------------------------------------------- */
/* the settings both windows share
 *
 * Grouped the way a clip goes through a flow: where it comes from, where it
 * goes, what the list does with it (or, for an instant replay, what playback
 * does as it lands), and how much of the buffer it is. */

class WarpFlowSettings : public QWidget {
public:
	std::function<void()> changed;

	WarpFlowSettings(QWidget *parent, QWidget *nameField) : QWidget(parent), creating(nameField != nullptr)
	{
		grid = new QGridLayout(this);
		grid->setColumnStretch(1, 1);
		grid->setHorizontalSpacing(20);
		grid->setVerticalSpacing(12);

		auto notify = [this]() {
			if (changed)
				changed();
		};

		if (nameField)
			addSection("Warp.Flow.Section.Flow",
				   warp_labelled(new QLabel(warp_flow_text("Warp.Flow.Name"), this), nameField, this));

		/* clips in */
		takes = new WarpSegment(this);
		takes->add(warp_flow_text("Warp.Flow.Trigger.Listen"), WARP_FLOW_TRIGGER_LISTEN);
		takes->add(warp_flow_text("Warp.Flow.Trigger.Hotkey"), WARP_FLOW_TRIGGER_HOTKEY);
		takes->changed = notify;

		fedBy = new QComboBox(this);
		fedBy->addItem(warp_flow_text("Warp.Flow.FedBy.None"), QString());
		fedByBox = warp_labelled(new QLabel(warp_flow_text("Warp.Flow.FedBy"), this), fedBy, this);
		fedBy->setToolTip(warp_flow_text("Warp.Flow.FedBy.Desc"));
		connect(fedBy, &QComboBox::currentIndexChanged, this, notify);

		auto *in = new QWidget(this);
		auto *inColumn = new QVBoxLayout(in);

		inColumn->setContentsMargins(0, 0, 0, 0);
		inColumn->addWidget(warp_labelled(new QLabel(warp_flow_text("Warp.Flow.Trigger"), this),
						  warp_hbox(this, {takes}), this));
		inColumn->addWidget(warp_hbox(this, {fedByBox}));
		addSection("Warp.Flow.Section.In", in);

		/* clips out: one source, or the two a combo feeds side by side */
		target = new QComboBox(this);
		newTarget = new QLineEdit(this);
		hlTarget = new QComboBox(this);
		hlNewTarget = new QLineEdit(this);
		targetLabel = new QLabel(this);
		newTarget->setPlaceholderText(warp_flow_text("Warp.Flow.Target.NewName.Placeholder"));
		hlNewTarget->setPlaceholderText(warp_flow_text("Warp.Flow.Target.NewName.Placeholder"));

		auto *out = new QWidget(this);
		auto *outGrid = new QGridLayout(out);

		outGrid->setContentsMargins(0, 0, 0, 0);
		outGrid->addWidget(targetLabel, 0, 0);
		outGrid->addWidget(target, 1, 0);
		outGrid->addWidget(newTarget, 2, 0);
		hlColumn =
			warp_labelled(new QLabel(warp_flow_text("Warp.Flow.Target.Highlight"), this), hlTarget, this);
		hlColumn->layout()->addWidget(hlNewTarget);
		outGrid->addWidget(hlColumn, 0, 1, 3, 1, Qt::AlignTop);
		outGrid->setColumnStretch(0, 1);
		outGrid->setColumnStretch(1, 1);
		addSection("Warp.Flow.Section.Out", out);

		connect(target, &QComboBox::currentIndexChanged, this, [this, notify](int) {
			showNewNames();
			notify();
		});
		connect(hlTarget, &QComboBox::currentIndexChanged, this, [this, notify](int) {
			showNewNames();
			notify();
		});
		connect(newTarget, &QLineEdit::textEdited, this, notify);
		connect(hlNewTarget, &QLineEdit::textEdited, this, notify);

		/* the list */
		order = orderSegment(notify);
		hlOrder = orderSegment(notify);
		orderLabel = new QLabel(this);
		hlOrderBox =
			warp_labelled(new QLabel(warp_flow_text("Warp.Flow.Order.Highlight"), this), hlOrder, this);

		limitCheck = new QCheckBox(warp_flow_text("Warp.Flow.Limit"), this);
		limitSpin = new QSpinBox(this);
		limitSpin->setRange(1, 500);
		limitSpin->setValue(10);
		limitSpin->setSuffix(warp_flow_text("Warp.Flow.Limit.Suffix"));
		limitBox = warp_labelled(new QLabel(warp_flow_text("Warp.Flow.Limit.Label"), this),
					 warp_hbox(this, {limitCheck, limitSpin}), this);
		connect(limitCheck, &QCheckBox::toggled, this, [this, notify](bool) {
			showEnabled();
			notify();
		});
		connect(limitSpin, &QSpinBox::valueChanged, this, notify);

		listSection =
			addSection("Warp.Flow.Section.List",
				   warp_hbox(this, {warp_labelled(orderLabel, order, this), hlOrderBox, limitBox}));

		/* what an instant replay does as a clip lands */
		playback = new WarpSegment(this);
		playback->add(warp_flow_text("Warp.Flow.Playback.Keep.Short"), WARP_MEDIA_LOAD_KEEP,
			      warp_flow_text("Warp.Flow.Playback.Keep"));
		playback->add(warp_flow_text("Warp.Flow.Playback.Play.Short"), WARP_MEDIA_LOAD_PLAY,
			      warp_flow_text("Warp.Flow.Playback.Play"));
		playback->add(warp_flow_text("Warp.Flow.Playback.Hold.Short"), WARP_MEDIA_LOAD_HOLD,
			      warp_flow_text("Warp.Flow.Playback.Hold"));
		playback->changed = notify;

		speedCheck = new QCheckBox(warp_flow_text("Warp.Flow.Speed"), this);
		speedSpin = new QSpinBox(this);
		speedSpin->setRange(WARP_FLOW_SPEED_MIN, WARP_FLOW_SPEED_MAX);
		speedSpin->setValue(50);
		speedSpin->setSuffix(warp_flow_text("Warp.Flow.Speed.Suffix"));
		connect(speedCheck, &QCheckBox::toggled, this, [this, notify](bool) {
			showEnabled();
			notify();
		});
		connect(speedSpin, &QSpinBox::valueChanged, this, notify);

		auto *lands = new QWidget(this);
		auto *landsColumn = new QVBoxLayout(lands);

		landsColumn->setContentsMargins(0, 0, 0, 0);
		landsColumn->addWidget(warp_hbox(this, {playback}));
		landsColumn->addWidget(warp_hbox(this, {speedCheck, speedSpin}));
		landsSection = addSection("Warp.Flow.Section.Lands", lands);

		/* how much of the buffer, and the other lengths on offer */
		lengthMode = new WarpSegment(this);
		lengthMode->add(warp_flow_text("Warp.Flow.Length.Whole.Short"), "whole",
				warp_flow_text("Warp.Flow.Length.Desc"));
		lengthMode->add(warp_flow_text("Warp.Flow.Length.Last"), "last",
				warp_flow_text("Warp.Flow.Length.Desc"));
		lengthMode->changed = [this, notify]() {
			showEnabled();
			notify();
		};
		lengthSpin = new QSpinBox(this);
		lengthSpin->setRange(1, WARP_FLOW_LENGTH_MAX);
		lengthSpin->setValue(20);
		lengthSpin->setSuffix(warp_flow_text("Warp.Flow.Length.Suffix"));
		connect(lengthSpin, &QSpinBox::valueChanged, this, notify);

		tags = new QHBoxLayout();
		tags->setContentsMargins(0, 0, 0, 0);
		addLength = new QPushButton(QStringLiteral("+ ") + warp_flow_text("Warp.Flow.Lengths.Add"), this);
		tags->addWidget(addLength);
		tags->addStretch(1);
		connect(addLength, &QPushButton::clicked, this, [this, notify]() {
			bool ok = false;
			int seconds = QInputDialog::getInt(this, warp_flow_text("Warp.Flow.Lengths.Add"),
							   warp_flow_text("Warp.Flow.Lengths.Prompt"), 10, 1,
							   WARP_FLOW_LENGTH_MAX, 1, &ok);

			if (!ok || lengths.contains(seconds))
				return;

			lengths.append(seconds);
			std::sort(lengths.begin(), lengths.end());
			showLengths();
			notify();
		});

		auto *length = new QWidget(this);
		auto *lengthColumn = new QVBoxLayout(length);

		lengthColumn->setContentsMargins(0, 0, 0, 0);
		lengthColumn->addWidget(warp_hbox(this, {lengthMode, lengthSpin}));
		lengthColumn->addWidget(new QLabel(warp_flow_text("Warp.Flow.Lengths"), this));
		lengthColumn->addLayout(tags);
		lengthColumn->itemAt(1)->widget()->setToolTip(warp_flow_text("Warp.Flow.Lengths.Desc"));
		addSection("Warp.Flow.Section.Length", length);

		grid->setRowStretch(grid->rowCount(), 1);
		showLengths();
	}

	void setKind(const QString &value)
	{
		const bool combo = value == WARP_FLOW_KIND_COMBO;
		const bool highlight = value == WARP_FLOW_KIND_HIGHLIGHT;
		const bool instant = value == WARP_FLOW_KIND_INSTANT;
		const QString source_id = QString::fromUtf8(warp_flow_target_id(value));

		/* an instant replay is held in a Warp Media source, so it is
		 * picked from other sources; refilled only when that changes,
		 * so moving between the list kinds keeps what was chosen */
		if (source_id != targetSourceId) {
			QSignalBlocker block(target);

			targetSourceId = source_id;
			warp_fill_target_combo(target, QString(), source_id.toUtf8().constData());
		}

		if (creating && hlTarget->count() == 0) {
			QSignalBlocker block(hlTarget);

			warp_fill_target_combo(hlTarget, QString(), WARP_PLAYLIST_SOURCE_ID);
			hlTarget->setCurrentIndex(hlTarget->count() - 1);
		}

		targetLabel->setText(warp_flow_text(combo     ? "Warp.Flow.Target.Replay"
						    : instant ? "Warp.Flow.Target.Instant"
							      : "Warp.Flow.Target"));
		orderLabel->setText(warp_flow_text(combo ? "Warp.Flow.Order.Replay" : "Warp.Flow.Order"));

		fedByBox->setVisible(creating && (highlight || instant));
		hlColumn->setVisible(combo);
		hlOrderBox->setVisible(combo);
		/* a highlight list keeps what it is given: the clips it is fed
		 * have been picked out already */
		limitBox->setVisible(!highlight && !instant);
		setSectionVisible(listSection, !instant);
		setSectionVisible(landsSection, instant);
		showNewNames();
	}

	void load(obs_data_t *config)
	{
		QSignalBlocker block(target);
		const int max_clips = (int)obs_data_get_int(config, WARP_FLOW_MAX_CLIPS);
		const int speed = (int)obs_data_get_int(config, WARP_FLOW_SPEED);
		const int length = (int)obs_data_get_int(config, WARP_FLOW_LENGTH);
		obs_data_array_t *array = obs_data_get_array(config, WARP_FLOW_LENGTHS);

		warp_fill_target_combo(target, QString::fromUtf8(obs_data_get_string(config, WARP_FLOW_TARGET_UUID)),
				       targetSourceId.toUtf8().constData());
		newTarget->clear();
		takes->setValue(QString::fromUtf8(obs_data_get_string(config, WARP_FLOW_TRIGGER)));
		order->setValue(QString::fromUtf8(obs_data_get_string(config, WARP_FLOW_ORDER)));
		playback->setValue(QString::fromUtf8(obs_data_get_string(config, WARP_FLOW_PLAYBACK)));
		limitCheck->setChecked(max_clips > 0);
		if (max_clips > 0)
			limitSpin->setValue(max_clips);
		speedCheck->setChecked(speed > 0);
		if (speed > 0)
			speedSpin->setValue(speed);
		lengthMode->setValue(length > 0 ? "last" : "whole");
		if (length > 0)
			lengthSpin->setValue(length);

		lengths.clear();
		for (size_t i = 0; array && i < obs_data_array_count(array); i++) {
			obs_data_t *item = obs_data_array_item(array, i);

			lengths.append((int)obs_data_get_int(item, WARP_FLOW_LENGTH_SECONDS));
			obs_data_release(item);
		}
		obs_data_array_release(array);

		showLengths();
		showNewNames();
	}

	/* what both windows write the same way */
	void write(obs_data_t *config, bool instant) const
	{
		obs_data_set_int(config, WARP_FLOW_LENGTH, length());
		warp_set_lengths(config, lengths);

		/* what playback does with a clip, and how fast, is the instant
		 * replay flow's alone */
		if (instant) {
			obs_data_set_string(config, WARP_FLOW_PLAYBACK, playback->value().toUtf8().constData());
			obs_data_set_int(config, WARP_FLOW_SPEED, speedCheck->isChecked() ? speedSpin->value() : 0);
		}
	}

	int length() const { return lengthMode->value() == "last" ? lengthSpin->value() : WARP_FLOW_LENGTH_WHOLE; }
	int maxClips() const { return limitCheck->isChecked() ? limitSpin->value() : 0; }

	/* the name a source made for the flow gets */
	QString targetText(QComboBox *combo, QLineEdit *edit, const QString &fallback) const
	{
		if (!combo->currentData().toString().isEmpty())
			return combo->currentText();

		return edit->text().trimmed().isEmpty() ? fallback : edit->text().trimmed();
	}

	/* the Save hotkeys a flow with these settings has, by name */
	QStringList saveHotkeys() const
	{
		QStringList names;

		names.append(warp_flow_text("Warp.Window.Menu.Save") + " (" +
			     (length() > 0 ? warp_length_label(length()) : warp_flow_text("Warp.Window.Length.Whole")) +
			     ")");
		for (int seconds : lengths)
			names.append(warp_flow_text("Warp.Window.Menu.SaveLength").arg(warp_length_label(seconds)));

		return names;
	}

	WarpSegment *takes = nullptr;
	WarpSegment *order = nullptr;
	WarpSegment *hlOrder = nullptr;
	WarpSegment *playback = nullptr;
	QComboBox *fedBy = nullptr;
	QComboBox *target = nullptr;
	QLineEdit *newTarget = nullptr;
	QComboBox *hlTarget = nullptr;
	QLineEdit *hlNewTarget = nullptr;
	QList<int> lengths;
	QString targetSourceId;

private:
	WarpSegment *orderSegment(const std::function<void()> &notify)
	{
		auto *segment = new WarpSegment(this);

		segment->add(warp_flow_text("Warp.Flow.Order.OldestFirst"), WARP_FLOW_ORDER_OLDEST_FIRST);
		segment->add(warp_flow_text("Warp.Flow.Order.NewestFirst"), WARP_FLOW_ORDER_NEWEST_FIRST);
		segment->changed = notify;
		return segment;
	}

	/* a titled section under a rule, answering its row */
	int addSection(const char *title, QWidget *content)
	{
		int row = grid->rowCount();

		if (row > 0 || grid->count() > 0) {
			auto *rule = new QFrame(this);

			rule->setFrameShape(QFrame::HLine);
			rule->setFrameShadow(QFrame::Sunken);
			grid->addWidget(rule, row++, 0, 1, 2);
		}

		grid->addWidget(warp_section_title(warp_flow_text(title), this), row, 0);
		grid->addWidget(content, row, 1);
		return row;
	}

	void setSectionVisible(int row, bool visible)
	{
		for (int r = row - 1; r <= row; r++) {
			for (int c = 0; c < 2; c++) {
				QLayoutItem *item = grid->itemAtPosition(r, c);

				if (item && item->widget())
					item->widget()->setVisible(visible);
			}
		}
	}

	void showNewNames()
	{
		newTarget->setVisible(target->currentData().toString().isEmpty());
		hlNewTarget->setVisible(hlTarget->currentData().toString().isEmpty());
	}

	void showEnabled()
	{
		limitSpin->setEnabled(limitCheck->isChecked());
		speedSpin->setEnabled(speedCheck->isChecked());
		lengthSpin->setVisible(lengthMode->value() == "last");
	}

	/* each length on offer as a tag that is clicked to take it off */
	void showLengths()
	{
		while (tags->count() > 2) {
			QLayoutItem *item = tags->takeAt(0);

			delete item->widget();
			delete item;
		}

		for (int i = 0; i < lengths.size(); i++) {
			const int seconds = lengths[i];
			auto *tag = new QPushButton(warp_length_label(seconds) + QStringLiteral("  ×"), this);

			tag->setToolTip(warp_flow_text("Warp.Flow.Lengths.Remove").arg(warp_length_label(seconds)));
			tags->insertWidget(i, tag);
			connect(tag, &QPushButton::clicked, this, [this, seconds]() {
				lengths.removeAll(seconds);
				/* the tag that was clicked is still on the stack */
				QTimer::singleShot(0, this, [this]() { showLengths(); });
				if (changed)
					changed();
			});
		}

		addLength->setEnabled(lengths.size() < WARP_FLOW_LENGTHS_MAX);
		showEnabled();
	}

	bool creating;
	QGridLayout *grid = nullptr;
	QWidget *fedByBox = nullptr;
	QWidget *hlColumn = nullptr;
	QWidget *hlOrderBox = nullptr;
	QWidget *limitBox = nullptr;
	QLabel *targetLabel = nullptr;
	QLabel *orderLabel = nullptr;
	QCheckBox *limitCheck = nullptr;
	QSpinBox *limitSpin = nullptr;
	QCheckBox *speedCheck = nullptr;
	QSpinBox *speedSpin = nullptr;
	WarpSegment *lengthMode = nullptr;
	QSpinBox *lengthSpin = nullptr;
	QHBoxLayout *tags = nullptr;
	QPushButton *addLength = nullptr;
	int listSection = 0;
	int landsSection = 0;
};

namespace {

QScrollArea *warp_scroll(QWidget *content, QWidget *parent)
{
	auto *scroll = new QScrollArea(parent);

	scroll->setWidget(content);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	return scroll;
}

/* a link between flows, as the array they are saved as */
obs_data_array_t *warp_link_array(const QStringList &links)
{
	obs_data_array_t *array = obs_data_array_create();

	for (const QString &link : links) {
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, WARP_FLOW_LINK_ID, link.toUtf8().constData());
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	return array;
}

QStringList warp_links_of(obs_data_t *flow)
{
	QStringList links;
	obs_data_array_t *array = obs_data_get_array(flow, WARP_FLOW_LINKS);

	for (size_t i = 0; array && i < obs_data_array_count(array); i++) {
		obs_data_t *item = obs_data_array_item(array, i);

		links.append(QString::fromUtf8(obs_data_get_string(item, WARP_FLOW_LINK_ID)));
		obs_data_release(item);
	}

	obs_data_array_release(array);
	return links;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* making flows */

WarpFlowCreateDialog::WarpFlowCreateDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(warp_flow_text("Warp.Flow.Create.Title"));
	setMinimumSize(760, 520);
	resize(980, 680);

	/* the kinds, each with the first line of what it is */
	kindList = new QListWidget(this);
	kindList->setIconSize(QSize(32, 32));
	kindList->setFixedWidth(260);
	kindList->setWordWrap(true);
	kindList->setSpacing(3);

	for (const WarpFlowKind &kind : warp_flow_kinds) {
		const QString first = warp_flow_text(kind.desc_key).section(QStringLiteral("\n\n"), 0, 0);
		auto *item = new QListWidgetItem(warp_flow_icon(kind.id),
						 warp_flow_text(kind.name_key) + QStringLiteral("\n") + first,
						 kindList);

		item->setData(Qt::UserRole, QString::fromUtf8(kind.id));
	}

	kindTitle = new QLabel(this);
	QFont title_font = kindTitle->font();

	title_font.setBold(true);
	title_font.setPointSizeF(title_font.pointSizeF() * 1.25);
	kindTitle->setFont(title_font);

	kindDesc = new QLabel(this);
	kindDesc->setWordWrap(true);
	kindDesc->setTextFormat(Qt::PlainText);

	chain = new QLabel(this);
	chain->setWordWrap(true);
	chain->setTextFormat(Qt::PlainText);

	nameEdit = new QLineEdit(this);
	nameEdit->setPlaceholderText(warp_flow_text("Warp.Flow.Name.Placeholder"));
	settings = new WarpFlowSettings(this, nameEdit);

	/* the replay flows a highlight or instant replay can be fed from */
	obs_data_array_t *flows = warp_flow_list();

	for (size_t i = 0; i < obs_data_array_count(flows); i++) {
		obs_data_t *flow = obs_data_array_item(flows, i);

		if (strcmp(obs_data_get_string(flow, WARP_FLOW_KIND), WARP_FLOW_KIND_REPLAY) == 0)
			settings->fedBy->addItem(QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME)),
						 QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID)));

		obs_data_release(flow);
	}

	obs_data_array_release(flows);

	auto *about = new QWidget(this);
	auto *aboutColumn = new QVBoxLayout(about);

	aboutColumn->addWidget(kindTitle);
	aboutColumn->addWidget(kindDesc);
	aboutColumn->addWidget(chain);

	auto *page = new QWidget(this);
	auto *pageColumn = new QVBoxLayout(page);

	pageColumn->setContentsMargins(0, 0, 0, 0);
	pageColumn->addWidget(about);
	pageColumn->addWidget(settings, 1);

	auto *columns = new QHBoxLayout();

	columns->addWidget(kindList);
	columns->addWidget(warp_scroll(page, this), 1);

	summary = new QLabel(this);
	summary->setWordWrap(true);
	summary->setEnabled(false);
	createButton = new QPushButton(this);
	createButton->setDefault(true);
	auto *cancelButton = new QPushButton(warp_flow_text("Warp.Flow.Cancel"), this);

	auto *footer = new QHBoxLayout();

	footer->addWidget(summary, 1);
	footer->addWidget(cancelButton);
	footer->addWidget(createButton);

	auto *layout = new QVBoxLayout(this);

	layout->addLayout(columns, 1);
	layout->addLayout(footer);

	connect(kindList, &QListWidget::currentRowChanged, this, [this](int) { refresh(); });
	connect(nameEdit, &QLineEdit::textChanged, this, [this]() { refresh(); });
	settings->changed = [this]() {
		refresh();
	};
	connect(createButton, &QPushButton::clicked, this, &QDialog::accept);
	connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

	kindList->setCurrentRow(0);
	nameEdit->setFocus();
}

QString WarpFlowCreateDialog::kind() const
{
	QListWidgetItem *item = kindList->currentItem();

	return item ? item->data(Qt::UserRole).toString() : QString(WARP_FLOW_KIND_REPLAY);
}

/* Says what the kind is, what the settings as they stand will make and the
 * hotkeys it will add: the chain of where a clip goes, and the keys that run
 * it, before anything is made. */
void WarpFlowCreateDialog::refresh()
{
	const QString value = kind();
	const bool combo = value == WARP_FLOW_KIND_COMBO;
	const QString name = nameEdit->text().trimmed().isEmpty() ? warp_flow_text("Warp.Flow.Chain.This")
								  : nameEdit->text().trimmed();
	const QString arrow = QStringLiteral("  →  ");

	for (const WarpFlowKind &entry : warp_flow_kinds) {
		if (value == QString::fromUtf8(entry.id)) {
			kindTitle->setText(warp_flow_text(entry.name_key));
			kindDesc->setText(warp_flow_text(entry.desc_key).section(QStringLiteral("\n\n"), 1));
		}
	}

	settings->setKind(value);

	const QString from = value == WARP_FLOW_KIND_HIGHLIGHT && !settings->fedBy->currentData().toString().isEmpty()
				     ? settings->fedBy->currentText()
				     : warp_flow_text("Warp.Flow.Chain.Buffer");
	QString text = from + arrow + name + arrow + settings->targetText(settings->target, settings->newTarget, name);

	if (combo) {
		const QString hl = name + " " + warp_flow_text("Warp.Flow.Combo.HighlightSuffix");

		text += QStringLiteral("\n↳  ") + hl + arrow +
			settings->targetText(settings->hlTarget, settings->hlNewTarget, hl);
	}

	chain->setText(text);

	QStringList hotkeys = settings->saveHotkeys();

	hotkeys.append(warp_flow_text("Warp.Window.Menu.AddLast"));
	summary->setText(warp_flow_text(combo ? "Warp.Flow.Create.Hotkeys.Combo" : "Warp.Flow.Create.Hotkeys")
				 .arg(hotkeys.join(QStringLiteral(", "))));
	createButton->setText(warp_flow_text(combo ? "Warp.Flow.Create.AddBoth" : "Warp.Flow.Create.Add"));
}

bool WarpFlowCreateDialog::buildFlow(const char *kind, const QString &name, bool highlight, const char *trigger,
				     const char *order, int max_clips, const QList<int> &lengths,
				     const QStringList &links, QString &flow_id)
{
	QString uuid;
	QString target_name;
	const char *target_id = warp_flow_target_id(QString::fromUtf8(kind));

	if (!warp_resolve_target(this, highlight ? settings->hlTarget : settings->target,
				 highlight ? settings->hlNewTarget : settings->newTarget, name, target_id, uuid,
				 target_name))
		return false;

	obs_data_t *config = obs_data_create();

	obs_data_set_string(config, WARP_FLOW_NAME, name.toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_KIND, kind);
	obs_data_set_string(config, WARP_FLOW_TRIGGER, trigger);
	obs_data_set_string(config, WARP_FLOW_ORDER, order);
	obs_data_set_string(config, WARP_FLOW_TARGET_UUID, uuid.toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_TARGET_NAME, target_name.toUtf8().constData());
	obs_data_set_int(config, WARP_FLOW_MAX_CLIPS, max_clips);
	obs_data_set_bool(config, WARP_FLOW_ENABLED, true);

	/* the pair a Combo makes keeps the same length either side, and it is
	 * the replay list that takes the saves, so the length hotkeys are its */
	settings->write(config, warp_flow_target_is_media(target_id));
	warp_set_lengths(config, lengths);

	obs_data_array_t *link_array = warp_link_array(links);

	obs_data_set_array(config, WARP_FLOW_LINKS, link_array);
	obs_data_array_release(link_array);

	char *id = warp_flow_add(config);

	obs_data_release(config);

	if (!id)
		return false;

	flow_id = QString::fromUtf8(id);
	bfree(id);
	created.append(flow_id);

	return true;
}

/* Hands the flow that was just made to the flow feeding it, which is where the
 * link between the two lives: 'Fed by' is the other end of 'Also feeds'. */
void WarpFlowCreateDialog::linkFedBy(const QString &fedBy, const QString &flowId)
{
	if (fedBy.isEmpty())
		return;

	obs_data_t *source_flow = warp_flow_get(fedBy.toUtf8().constData());

	if (!source_flow)
		return;

	obs_data_array_t *links = warp_link_array(warp_links_of(source_flow) << flowId);
	obs_data_t *update = obs_data_create();

	obs_data_set_array(update, WARP_FLOW_LINKS, links);
	warp_flow_update(fedBy.toUtf8().constData(), update);

	obs_data_release(update);
	obs_data_array_release(links);
	obs_data_release(source_flow);
}

void WarpFlowCreateDialog::accept()
{
	const QString value = kind();
	const QString name = nameEdit->text().trimmed();

	if (name.isEmpty()) {
		QMessageBox::warning(this, warp_flow_text("Warp.Flow.Create.Title"),
				     warp_flow_text("Warp.Flow.Error.NameEmpty"));
		nameEdit->setFocus();
		return;
	}

	if (!warp_flow_name_free(this, name, QString()))
		return;

	const QByteArray trigger = settings->takes->value().toUtf8();
	const QByteArray order = settings->order->value().toUtf8();
	const QByteArray hl_order = settings->hlOrder->value().toUtf8();
	const QList<int> lengths = settings->lengths;

	if (value == WARP_FLOW_KIND_COMBO) {
		const QString hl_name = name + QStringLiteral(" ") + warp_flow_text("Warp.Flow.Combo.HighlightSuffix");
		QString hl_id;
		QString replay_id;

		if (!warp_flow_name_free(this, hl_name, QString()))
			return;

		/* the highlight list first: the replay list is made linked to
		 * it, so the pair works from the moment it is there */
		if (!buildFlow(WARP_FLOW_KIND_HIGHLIGHT, hl_name, true, WARP_FLOW_TRIGGER_HOTKEY, hl_order.constData(),
			       0, QList<int>(), QStringList(), hl_id))
			return;

		if (!buildFlow(WARP_FLOW_KIND_REPLAY, name, false, trigger.constData(), order.constData(),
			       settings->maxClips(), lengths, QStringList() << hl_id, replay_id)) {
			/* the highlight list was made for a pair that did not
			 * come off, so it goes back with it */
			for (const QString &made : created)
				warp_flow_remove(made.toUtf8().constData());

			created.clear();
			return;
		}
	} else if (value == WARP_FLOW_KIND_HIGHLIGHT || value == WARP_FLOW_KIND_INSTANT) {
		const QByteArray flow_kind = value.toUtf8();
		QString flow_id;

		if (!buildFlow(flow_kind.constData(), name, false, trigger.constData(), order.constData(), 0, lengths,
			       QStringList(), flow_id))
			return;

		/* fed by a replay flow: it is that flow that has to be told */
		linkFedBy(settings->fedBy->currentData().toString(), flow_id);
	} else {
		QString flow_id;

		if (!buildFlow(WARP_FLOW_KIND_REPLAY, name, false, trigger.constData(), order.constData(),
			       settings->maxClips(), lengths, QStringList(), flow_id))
			return;
	}

	QDialog::accept();
}

/* ------------------------------------------------------------------------- */
/* changing one */

WarpFlowPropsDialog::WarpFlowPropsDialog(QWidget *parent, const QString &id) : QDialog(parent), flowId(id)
{
	obs_data_t *config = warp_flow_get(id.toUtf8().constData());

	kind = QString::fromUtf8(config ? obs_data_get_string(config, WARP_FLOW_KIND) : WARP_FLOW_KIND_REPLAY);
	obs_data_release(config);

	setWindowTitle(warp_flow_text("Warp.Flow.Props.Title"));
	setMinimumSize(720, 500);
	resize(960, 640);

	/* who the flow is: its name, what it is and how it is doing, and
	 * whether it takes clips */
	auto *icon = new QLabel(this);

	icon->setPixmap(warp_flow_icon(kind.toUtf8().constData()).pixmap(QSize(36, 36), devicePixelRatioF()));

	nameEdit = new QLineEdit(this);
	QFont name_font = nameEdit->font();

	name_font.setBold(true);
	name_font.setPointSizeF(name_font.pointSizeF() * 1.3);
	nameEdit->setFont(name_font);
	nameEdit->setMaximumWidth(420);

	status = new QLabel(this);
	status->setEnabled(false);

	enabledText = new QLabel(this);
	enabledSwitch = new WarpSwitch(this);

	auto *identity = new QVBoxLayout();

	identity->addWidget(nameEdit);
	identity->addWidget(status);

	auto *header = new QHBoxLayout();

	header->addWidget(icon, 0, Qt::AlignTop);
	header->addLayout(identity, 1);
	header->addWidget(enabledText);
	header->addWidget(enabledSwitch);

	/* the sections down the side, and what is in them */
	sections = new QListWidget(this);
	sections->setFixedWidth(180);
	sections->addItems({warp_flow_text("Warp.Flow.Props.Settings"), warp_flow_text("Warp.Flow.Props.Links"),
			    warp_flow_text("Warp.Flow.Props.Hotkeys")});

	settings = new WarpFlowSettings(this, nullptr);
	settings->setKind(kind);

	auto *links = new QWidget(this);
	auto *linksColumn = new QVBoxLayout(links);
	auto *linksDesc = new QLabel(warp_flow_text("Warp.Flow.Links.Desc"), links);

	linkList = new QListWidget(links);
	linkList->setIconSize(QSize(24, 24));
	fedBy = new QLabel(links);
	fedBy->setWordWrap(true);
	linksDesc->setWordWrap(true);
	linksColumn->addWidget(warp_section_title(warp_flow_text("Warp.Flow.Links"), links));
	linksColumn->addWidget(linksDesc);
	linksColumn->addWidget(linkList, 1);
	linksColumn->addWidget(warp_section_title(warp_flow_text("Warp.Flow.Props.FedBy"), links));
	linksColumn->addWidget(fedBy);

	auto *hotkeys = new QWidget(this);
	auto *hotkeysColumn = new QVBoxLayout(hotkeys);
	auto *hotkeysDesc = new QLabel(warp_flow_text("Warp.Flow.Props.Hotkeys.Desc"), hotkeys);

	hotkeysDesc->setWordWrap(true);
	hotkeyForm = new QFormLayout();
	hotkeyForm->setHorizontalSpacing(24);
	hotkeysColumn->addWidget(warp_section_title(warp_flow_text("Warp.Flow.Props.Hotkeys"), hotkeys));
	hotkeysColumn->addWidget(hotkeysDesc);
	hotkeysColumn->addLayout(hotkeyForm);
	hotkeysColumn->addStretch(1);

	auto *pages = new QStackedWidget(this);

	pages->addWidget(warp_scroll(settings, this));
	pages->addWidget(links);
	pages->addWidget(hotkeys);

	auto *body = new QHBoxLayout();

	body->addWidget(sections);
	body->addWidget(pages, 1);

	/* the footer */
	dirtyLabel = new QLabel(this);
	revertButton = new QPushButton(warp_flow_text("Warp.Flow.Revert"), this);
	applyButton = new QPushButton(warp_flow_text("Warp.Flow.Apply"), this);
	applyButton->setDefault(true);
	auto *closeButton = new QPushButton(warp_flow_text("Warp.Flow.Close"), this);
	auto *footer = new QHBoxLayout();

	footer->addWidget(dirtyLabel, 1);
	footer->addWidget(revertButton);
	footer->addWidget(applyButton);
	footer->addSpacing(12);
	footer->addWidget(closeButton);

	auto *layout = new QVBoxLayout(this);

	layout->addLayout(header);
	layout->addLayout(body, 1);
	layout->addLayout(footer);

	auto changed = [this]() {
		setDirty(true);
	};

	settings->changed = changed;
	connect(nameEdit, &QLineEdit::textEdited, this, changed);
	connect(enabledSwitch, &QAbstractButton::toggled, this, [this, changed](bool on) {
		enabledText->setText(warp_flow_text(on ? "Warp.Flow.Props.On" : "Warp.Flow.Props.Off"));
		changed();
	});
	connect(linkList, &QListWidget::itemChanged, this, changed);
	connect(sections, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
	connect(revertButton, &QPushButton::clicked, this, [this]() { load(); });
	connect(applyButton, &QPushButton::clicked, this, [this]() { apply(); });
	connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

	/* the clip count moves as clips land */
	auto *timer = new QTimer(this);

	connect(timer, &QTimer::timeout, this, [this]() { refreshStatus(); });
	timer->start(1000);

	sections->setCurrentRow(0);
	load();
}

/* Puts the flow as it is saved into every field, which is also what Revert
 * does. */
void WarpFlowPropsDialog::load()
{
	obs_data_t *config = warp_flow_get(flowId.toUtf8().constData());

	if (!config)
		return;

	loading = true;

	nameEdit->setText(QString::fromUtf8(obs_data_get_string(config, WARP_FLOW_NAME)));
	enabledSwitch->setChecked(obs_data_get_bool(config, WARP_FLOW_ENABLED));
	enabledText->setText(warp_flow_text(enabledSwitch->isChecked() ? "Warp.Flow.Props.On" : "Warp.Flow.Props.Off"));
	settings->load(config);

	/* the flows this one hands its clips to, and the ones that hand theirs
	 * to it */
	const QStringList linked = warp_links_of(config);
	QStringList feeders;
	obs_data_array_t *flows = warp_flow_list();

	linkList->clear();

	for (size_t i = 0; i < obs_data_array_count(flows); i++) {
		obs_data_t *flow = obs_data_array_item(flows, i);
		const QString other = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID));
		const char *other_kind = obs_data_get_string(flow, WARP_FLOW_KIND);

		if (other != flowId) {
			const QString line = warp_flow_kind_name(other_kind) + QStringLiteral("  ·  ") +
					     QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_TARGET_NAME)) +
					     QStringLiteral("  ·  ") +
					     warp_flow_text(obs_data_get_bool(flow, WARP_FLOW_ENABLED)
								    ? "Warp.Flow.Props.On.Short"
								    : "Warp.Flow.Props.Off.Short");
			auto *item = new QListWidgetItem(warp_flow_icon(other_kind),
							 QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME)) +
								 QStringLiteral("\n") + line,
							 linkList);

			item->setData(Qt::UserRole, other);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(linked.contains(other) ? Qt::Checked : Qt::Unchecked);

			if (warp_links_of(flow).contains(flowId))
				feeders.append(QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME)));
		}

		obs_data_release(flow);
	}

	obs_data_array_release(flows);

	fedBy->setText(feeders.isEmpty() ? warp_flow_text("Warp.Flow.Props.FedBy.None")
					 : feeders.join(QStringLiteral(", ")));
	sections->item(1)->setText(warp_flow_text("Warp.Flow.Props.Links") +
				   QStringLiteral("  (%1)").arg(linked.size()));

	/* the keys that run it, as they are bound right now */
	const QMap<obs_hotkey_id, QStringList> keys = warp_bound_keys();
	struct warp_flow_hotkeys hotkeys;
	int rows = 0;

	while (hotkeyForm->rowCount())
		hotkeyForm->removeRow(0);

	auto keyRow = [&](const QString &label, obs_hotkey_id id) {
		const QString key = warp_key_text(keys, id);
		auto *value = new QLabel(key.isEmpty() ? warp_flow_text("Warp.Window.Hotkey.Unset") : key, this);

		value->setEnabled(!key.isEmpty());
		hotkeyForm->addRow(label, value);
		rows++;
	};

	if (warp_flow_get_hotkeys(flowId.toUtf8().constData(), &hotkeys)) {
		keyRow(warp_flow_text("Warp.Window.Menu.Save"), hotkeys.save);
		for (size_t i = 0; i < hotkeys.lengths_num; i++)
			keyRow(warp_flow_text("Warp.Window.Menu.SaveLength")
				       .arg(warp_length_label(hotkeys.length_seconds[i])),
			       hotkeys.length_hotkeys[i]);
		keyRow(warp_flow_text("Warp.Window.Menu.AddLast"), hotkeys.promote);
	}

	sections->item(2)->setText(warp_flow_text("Warp.Flow.Props.Hotkeys") + QStringLiteral("  (%1)").arg(rows));

	obs_data_release(config);

	loading = false;
	setDirty(false);
	refreshStatus();
}

void WarpFlowPropsDialog::refreshStatus()
{
	obs_data_t *config = warp_flow_get(flowId.toUtf8().constData());

	if (!config)
		return;

	const QString target = QString::fromUtf8(obs_data_get_string(config, WARP_FLOW_TARGET_NAME));
	const int clips = warp_flow_clip_count(flowId.toUtf8().constData());
	const int max = (int)obs_data_get_int(config, WARP_FLOW_MAX_CLIPS);
	QString text;

	if (clips < 0)
		text = warp_flow_text("Warp.Window.Feeds.Gone").arg(target);
	else if (kind == WARP_FLOW_KIND_INSTANT)
		text = warp_flow_text("Warp.Flow.Props.LoadsInto").arg(target) + QStringLiteral("  ·  ") +
		       warp_flow_text(clips > 0 ? "Warp.Window.Clips.Loaded" : "Warp.Window.Clips.Empty");
	else
		text = warp_flow_text("Warp.Flow.Props.Feeds").arg(target) + QStringLiteral("  ·  ") +
		       (max > 0 ? warp_flow_text("Warp.Flow.Props.ClipsOf").arg(clips).arg(max)
				: warp_flow_text("Warp.Flow.Props.Clips").arg(clips));

	status->setText(warp_flow_kind_name(kind.toUtf8().constData()) + QStringLiteral("  ·  ") + text);
	obs_data_release(config);
}

void WarpFlowPropsDialog::setDirty(bool value)
{
	if (loading)
		return;

	dirty = value;
	dirtyLabel->setText(warp_flow_text(dirty ? "Warp.Flow.Props.Unapplied" : "Warp.Flow.Props.Applied"));
	dirtyLabel->setEnabled(dirty);
	revertButton->setEnabled(dirty);
	applyButton->setEnabled(dirty);
}

bool WarpFlowPropsDialog::apply()
{
	const QString name = nameEdit->text().trimmed();

	if (name.isEmpty()) {
		QMessageBox::warning(this, warp_flow_text("Warp.Flow.Props.Title"),
				     warp_flow_text("Warp.Flow.Error.NameEmpty"));
		nameEdit->setFocus();
		return false;
	}

	if (!warp_flow_name_free(this, name, flowId))
		return false;

	QString uuid;
	QString target_name;

	if (!warp_resolve_target(this, settings->target, settings->newTarget, name,
				 settings->targetSourceId.toUtf8().constData(), uuid, target_name))
		return false;

	QStringList links;

	for (int i = 0; i < linkList->count(); i++) {
		if (linkList->item(i)->checkState() == Qt::Checked)
			links.append(linkList->item(i)->data(Qt::UserRole).toString());
	}

	obs_data_t *config = obs_data_create();
	obs_data_array_t *link_array = warp_link_array(links);

	obs_data_set_string(config, WARP_FLOW_NAME, name.toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_TARGET_UUID, uuid.toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_TARGET_NAME, target_name.toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_TRIGGER, settings->takes->value().toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_ORDER, settings->order->value().toUtf8().constData());
	obs_data_set_int(config, WARP_FLOW_MAX_CLIPS, settings->maxClips());
	obs_data_set_bool(config, WARP_FLOW_ENABLED, enabledSwitch->isChecked());
	obs_data_set_array(config, WARP_FLOW_LINKS, link_array);
	settings->write(config, kind == WARP_FLOW_KIND_INSTANT);

	warp_flow_update(flowId.toUtf8().constData(), config);

	obs_data_array_release(link_array);
	obs_data_release(config);

	load();
	return true;
}

/* Unapplied changes are applied or let go of before the window goes. */
void WarpFlowPropsDialog::closeEvent(QCloseEvent *event)
{
	if (dirty) {
		const auto answer = QMessageBox::question(
			this, windowTitle(), warp_flow_text("Warp.Flow.Props.Leave").arg(nameEdit->text().trimmed()),
			QMessageBox::Apply | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Apply);

		if (answer == QMessageBox::Cancel || (answer == QMessageBox::Apply && !apply())) {
			event->ignore();
			return;
		}
	}

	event->accept();
}

void WarpFlowPropsDialog::reject()
{
	close();
}
