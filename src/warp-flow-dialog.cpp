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

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QPolygonF>
#include <QSpinBox>
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

/* hides a form row, label and all: a field on its own leaves its label behind */
void warp_set_row_visible(QFormLayout *form, QWidget *field, bool visible)
{
	if (!form || !field)
		return;

	if (QWidget *label = form->labelForField(field))
		label->setVisible(visible);

	field->setVisible(visible);
}

bool warp_enum_playlist_sources(void *param, obs_source_t *source)
{
	const char *id = obs_source_get_id(source);

	if (id && strcmp(id, WARP_PLAYLIST_SOURCE_ID) == 0) {
		auto *out = static_cast<QList<QPair<QString, QString>> *>(param);
		const char *name = obs_source_get_name(source);
		const char *uuid = obs_source_get_uuid(source);

		out->append({QString::fromUtf8(name ? name : ""), QString::fromUtf8(uuid ? uuid : "")});
	}

	return true;
}

/* Every Warp Playlist source in the scene collection, with an entry for making
 * one on the end of it. The entry that is picked carries the source's uuid, or
 * an empty string for the new one. */
void warp_fill_target_combo(QComboBox *combo, const QString &selected_uuid)
{
	QList<QPair<QString, QString>> sources;

	obs_enum_sources(warp_enum_playlist_sources, &sources);
	std::sort(sources.begin(), sources.end(),
		  [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
			  return a.first.localeAwareCompare(b.first) < 0;
		  });

	combo->clear();

	for (const auto &source : sources)
		combo->addItem(source.first, source.second);

	combo->addItem(warp_flow_text("Warp.Flow.Target.New"), QString());

	int index = selected_uuid.isEmpty() ? -1 : combo->findData(selected_uuid);

	/* a playlist to feed has to be made when there is not one yet */
	if (index < 0)
		index = sources.isEmpty() ? combo->count() - 1 : 0;

	combo->setCurrentIndex(index);
}

void warp_fill_trigger_combo(QComboBox *combo)
{
	combo->addItem(warp_flow_text("Warp.Flow.Trigger.Hotkey"), QString(WARP_FLOW_TRIGGER_HOTKEY));
	combo->addItem(warp_flow_text("Warp.Flow.Trigger.Listen"), QString(WARP_FLOW_TRIGGER_LISTEN));
}

void warp_fill_order_combo(QComboBox *combo)
{
	combo->addItem(warp_flow_text("Warp.Flow.Order.OldestFirst"), QString(WARP_FLOW_ORDER_OLDEST_FIRST));
	combo->addItem(warp_flow_text("Warp.Flow.Order.NewestFirst"), QString(WARP_FLOW_ORDER_NEWEST_FIRST));
}

void warp_select_data(QComboBox *combo, const QString &value)
{
	const int index = combo->findData(value);

	if (index >= 0)
		combo->setCurrentIndex(index);
}

/* A Warp Playlist source called 'wanted', or as close to it as a name that is
 * not taken already gets. It is put in the current scene: a source no scene
 * holds on to is not saved with the scene collection. */
obs_source_t *warp_create_playlist_source(const QString &wanted)
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
	obs_source_t *source = obs_source_create(WARP_PLAYLIST_SOURCE_ID, name.toUtf8().constData(), settings, nullptr);

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

/* The playlist the flow is to feed, made first when that is what was asked
 * for. Answers false, having said why, when there is nothing to feed. */
bool warp_resolve_target(QWidget *parent, QComboBox *combo, QLineEdit *new_name, const QString &fallback_name,
			 QString &uuid, QString &name)
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

	obs_source_t *source = warp_create_playlist_source(wanted);

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

QWidget *warp_limit_row(QCheckBox **check, QSpinBox **spin, QWidget *parent)
{
	auto *row = new QWidget(parent);
	auto *layout = new QHBoxLayout(row);

	layout->setContentsMargins(0, 0, 0, 0);

	*check = new QCheckBox(warp_flow_text("Warp.Flow.Limit"), row);
	*spin = new QSpinBox(row);

	(*spin)->setRange(1, 500);
	(*spin)->setValue(10);
	(*spin)->setSuffix(warp_flow_text("Warp.Flow.Limit.Suffix"));
	(*spin)->setEnabled(false);

	QObject::connect(*check, &QCheckBox::toggled, *spin, &QSpinBox::setEnabled);

	layout->addWidget(*check);
	layout->addWidget(*spin);
	layout->addStretch(1);

	return row;
}

QFrame *warp_separator(QWidget *parent)
{
	auto *line = new QFrame(parent);

	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);

	return line;
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

	if (highlight) {
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

/* ------------------------------------------------------------------------- */
/* making flows */

WarpFlowCreateDialog::WarpFlowCreateDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(warp_flow_text("Warp.Flow.Create.Title"));
	resize(820, 560);

	kindList = new QListWidget(this);
	kindList->setIconSize(QSize(36, 36));
	kindList->setMinimumWidth(210);
	kindList->setMaximumWidth(240);
	kindList->setUniformItemSizes(false);

	for (const WarpFlowKind &kind : warp_flow_kinds) {
		auto *item = new QListWidgetItem(warp_flow_icon(kind.id), warp_flow_text(kind.name_key), kindList);

		item->setData(Qt::UserRole, QString::fromUtf8(kind.id));
	}

	kindTitle = new QLabel(this);

	QFont title_font = kindTitle->font();

	title_font.setBold(true);
	title_font.setPointSizeF(title_font.pointSizeF() * 1.2);
	kindTitle->setFont(title_font);

	kindDesc = new QLabel(this);
	kindDesc->setWordWrap(true);
	kindDesc->setTextFormat(Qt::PlainText);
	kindDesc->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	kindDesc->setMinimumHeight(90);

	form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	nameEdit = new QLineEdit(this);
	nameEdit->setPlaceholderText(warp_flow_text("Warp.Flow.Name.Placeholder"));
	form->addRow(warp_flow_text("Warp.Flow.Name"), nameEdit);

	targetCombo = new QComboBox(this);
	warp_fill_target_combo(targetCombo, QString());
	form->addRow(warp_flow_text("Warp.Flow.Target"), targetCombo);

	newTargetEdit = new QLineEdit(this);
	newTargetEdit->setPlaceholderText(warp_flow_text("Warp.Flow.Target.NewName.Placeholder"));
	form->addRow(warp_flow_text("Warp.Flow.Target.NewName"), newTargetEdit);

	hlTargetCombo = new QComboBox(this);
	warp_fill_target_combo(hlTargetCombo, QString());
	hlTargetCombo->setCurrentIndex(hlTargetCombo->count() - 1);
	form->addRow(warp_flow_text("Warp.Flow.Target.Highlight"), hlTargetCombo);

	hlNewTargetEdit = new QLineEdit(this);
	hlNewTargetEdit->setPlaceholderText(warp_flow_text("Warp.Flow.Target.NewName.Placeholder"));
	form->addRow(warp_flow_text("Warp.Flow.Target.Highlight.NewName"), hlNewTargetEdit);

	fedByCombo = new QComboBox(this);
	form->addRow(warp_flow_text("Warp.Flow.FedBy"), fedByCombo);

	triggerCombo = new QComboBox(this);
	warp_fill_trigger_combo(triggerCombo);
	form->addRow(warp_flow_text("Warp.Flow.Trigger"), triggerCombo);

	orderCombo = new QComboBox(this);
	warp_fill_order_combo(orderCombo);
	form->addRow(warp_flow_text("Warp.Flow.Order"), orderCombo);

	hlOrderCombo = new QComboBox(this);
	warp_fill_order_combo(hlOrderCombo);
	form->addRow(warp_flow_text("Warp.Flow.Order.Highlight"), hlOrderCombo);

	QWidget *limit_row = warp_limit_row(&limitCheck, &limitSpin, this);

	form->addRow(warp_flow_text("Warp.Flow.Limit.Label"), limit_row);

	/* the replay flows a highlight list can be fed from */
	fedByCombo->addItem(warp_flow_text("Warp.Flow.FedBy.None"), QString());

	obs_data_array_t *flows = warp_flow_list();
	const size_t flow_count = obs_data_array_count(flows);

	for (size_t i = 0; i < flow_count; i++) {
		obs_data_t *flow = obs_data_array_item(flows, i);

		if (strcmp(obs_data_get_string(flow, WARP_FLOW_KIND), WARP_FLOW_KIND_REPLAY) == 0)
			fedByCombo->addItem(QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME)),
					    QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID)));

		obs_data_release(flow);
	}

	obs_data_array_release(flows);

	auto *right = new QVBoxLayout();

	right->addWidget(kindTitle);
	right->addWidget(kindDesc);
	right->addWidget(warp_separator(this));
	right->addLayout(form);
	right->addStretch(1);

	auto *columns = new QHBoxLayout();

	columns->addWidget(kindList);
	columns->addLayout(right, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);

	layout->addLayout(columns, 1);
	layout->addWidget(buttons);

	connect(kindList, &QListWidget::currentRowChanged, this, [this](int) { kindChanged(); });
	connect(targetCombo, &QComboBox::currentIndexChanged, this, [this](int) { targetChanged(); });
	connect(hlTargetCombo, &QComboBox::currentIndexChanged, this, [this](int) { targetChanged(); });

	kindList->setCurrentRow(0);
	nameEdit->setFocus();
}

void WarpFlowCreateDialog::kindChanged()
{
	QListWidgetItem *item = kindList->currentItem();

	if (!item)
		return;

	const QString kind = item->data(Qt::UserRole).toString();
	const bool is_combo = kind == QString(WARP_FLOW_KIND_COMBO);
	const bool is_highlight = kind == QString(WARP_FLOW_KIND_HIGHLIGHT);

	for (const WarpFlowKind &entry : warp_flow_kinds) {
		if (kind == QString::fromUtf8(entry.id)) {
			kindTitle->setText(warp_flow_text(entry.name_key));
			kindDesc->setText(warp_flow_text(entry.desc_key));
			break;
		}
	}

	warp_set_row_visible(form, hlTargetCombo, is_combo);
	warp_set_row_visible(form, hlOrderCombo, is_combo);
	warp_set_row_visible(form, fedByCombo, is_highlight);

	/* A highlight list keeps what it is given: the clips it is fed have
	 * been picked out already, so a limit on it would throw them away. */
	warp_set_row_visible(form, limitCheck->parentWidget(), !is_highlight);

	if (auto *label = qobject_cast<QLabel *>(form->labelForField(targetCombo)))
		label->setText(warp_flow_text(is_combo ? "Warp.Flow.Target.Replay" : "Warp.Flow.Target"));

	targetChanged();
}

void WarpFlowCreateDialog::targetChanged()
{
	QListWidgetItem *item = kindList->currentItem();
	const QString kind = item ? item->data(Qt::UserRole).toString() : QString();
	const bool is_combo = kind == QString(WARP_FLOW_KIND_COMBO);

	warp_set_row_visible(form, newTargetEdit, targetCombo->currentData().toString().isEmpty());
	warp_set_row_visible(form, hlNewTargetEdit, is_combo && hlTargetCombo->currentData().toString().isEmpty());
}

bool WarpFlowCreateDialog::buildFlow(const char *kind, const QString &name, QComboBox *target_combo,
				     QLineEdit *new_target_edit, const char *trigger, const char *order, int max_clips,
				     const QStringList &links, QString &flow_id)
{
	QString uuid;
	QString target_name;

	if (!warp_resolve_target(this, target_combo, new_target_edit, name, uuid, target_name))
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

	obs_data_array_t *link_array = obs_data_array_create();

	for (const QString &link : links) {
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, WARP_FLOW_LINK_ID, link.toUtf8().constData());
		obs_data_array_push_back(link_array, item);
		obs_data_release(item);
	}

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

void WarpFlowCreateDialog::accept()
{
	QListWidgetItem *item = kindList->currentItem();

	if (!item)
		return;

	const QString kind = item->data(Qt::UserRole).toString();
	const QString name = nameEdit->text().trimmed();

	if (name.isEmpty()) {
		QMessageBox::warning(this, warp_flow_text("Warp.Flow.Create.Title"),
				     warp_flow_text("Warp.Flow.Error.NameEmpty"));
		nameEdit->setFocus();
		return;
	}

	if (!warp_flow_name_free(this, name, QString()))
		return;

	const QByteArray trigger = triggerCombo->currentData().toString().toUtf8();
	const QByteArray order = orderCombo->currentData().toString().toUtf8();
	const QByteArray hl_order = hlOrderCombo->currentData().toString().toUtf8();
	const int max_clips = limitCheck->isChecked() ? limitSpin->value() : 0;

	if (kind == QString(WARP_FLOW_KIND_COMBO)) {
		const QString hl_name = name + QStringLiteral(" ") + warp_flow_text("Warp.Flow.Combo.HighlightSuffix");
		QString hl_id;
		QString replay_id;

		if (!warp_flow_name_free(this, hl_name, QString()))
			return;

		/* the highlight list first: the replay list is made linked to
		 * it, so the pair works from the moment it is there */
		if (!buildFlow(WARP_FLOW_KIND_HIGHLIGHT, hl_name, hlTargetCombo, hlNewTargetEdit,
			       WARP_FLOW_TRIGGER_HOTKEY, hl_order.constData(), 0, QStringList(), hl_id))
			return;

		if (!buildFlow(WARP_FLOW_KIND_REPLAY, name, targetCombo, newTargetEdit, trigger.constData(),
			       order.constData(), max_clips, QStringList() << hl_id, replay_id)) {
			/* the highlight list was made for a pair that did not
			 * come off, so it goes back with it */
			for (const QString &made : created)
				warp_flow_remove(made.toUtf8().constData());

			created.clear();
			return;
		}
	} else if (kind == QString(WARP_FLOW_KIND_HIGHLIGHT)) {
		QString flow_id;

		if (!buildFlow(WARP_FLOW_KIND_HIGHLIGHT, name, targetCombo, newTargetEdit, trigger.constData(),
			       order.constData(), 0, QStringList(), flow_id))
			return;

		/* fed by a replay flow: it is that flow that has to be told */
		const QString fed_by = fedByCombo->currentData().toString();

		if (!fed_by.isEmpty()) {
			obs_data_t *source_flow = warp_flow_get(fed_by.toUtf8().constData());

			if (source_flow) {
				obs_data_array_t *links = obs_data_get_array(source_flow, WARP_FLOW_LINKS);

				if (!links)
					links = obs_data_array_create();

				obs_data_t *link = obs_data_create();

				obs_data_set_string(link, WARP_FLOW_LINK_ID, flow_id.toUtf8().constData());
				obs_data_array_push_back(links, link);
				obs_data_release(link);

				obs_data_t *update = obs_data_create();

				obs_data_set_array(update, WARP_FLOW_LINKS, links);
				warp_flow_update(fed_by.toUtf8().constData(), update);

				obs_data_release(update);
				obs_data_array_release(links);
				obs_data_release(source_flow);
			}
		}
	} else {
		QString flow_id;

		if (!buildFlow(WARP_FLOW_KIND_REPLAY, name, targetCombo, newTargetEdit, trigger.constData(),
			       order.constData(), max_clips, QStringList(), flow_id))
			return;
	}

	QDialog::accept();
}

/* ------------------------------------------------------------------------- */
/* changing one */

WarpFlowPropsDialog::WarpFlowPropsDialog(QWidget *parent, const QString &id) : QDialog(parent), flowId(id)
{
	obs_data_t *config = warp_flow_get(id.toUtf8().constData());

	setWindowTitle(warp_flow_text("Warp.Flow.Props.Title"));
	resize(560, 520);

	const char *kind = config ? obs_data_get_string(config, WARP_FLOW_KIND) : WARP_FLOW_KIND_REPLAY;
	const bool is_highlight = strcmp(kind, WARP_FLOW_KIND_HIGHLIGHT) == 0;

	auto *header = new QHBoxLayout();
	auto *icon = new QLabel(this);

	icon->setPixmap(warp_flow_icon(kind).pixmap(32, 32));

	auto *kind_label = new QLabel(warp_flow_kind_name(kind), this);
	QFont kind_font = kind_label->font();

	kind_font.setBold(true);
	kind_label->setFont(kind_font);

	header->addWidget(icon);
	header->addWidget(kind_label);
	header->addStretch(1);

	form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	nameEdit = new QLineEdit(this);
	nameEdit->setText(QString::fromUtf8(config ? obs_data_get_string(config, WARP_FLOW_NAME) : ""));
	form->addRow(warp_flow_text("Warp.Flow.Name"), nameEdit);

	targetCombo = new QComboBox(this);
	warp_fill_target_combo(targetCombo,
			       QString::fromUtf8(config ? obs_data_get_string(config, WARP_FLOW_TARGET_UUID) : ""));
	form->addRow(warp_flow_text("Warp.Flow.Target"), targetCombo);

	newTargetEdit = new QLineEdit(this);
	newTargetEdit->setPlaceholderText(warp_flow_text("Warp.Flow.Target.NewName.Placeholder"));
	form->addRow(warp_flow_text("Warp.Flow.Target.NewName"), newTargetEdit);

	triggerCombo = new QComboBox(this);
	warp_fill_trigger_combo(triggerCombo);
	warp_select_data(triggerCombo, QString::fromUtf8(config ? obs_data_get_string(config, WARP_FLOW_TRIGGER) : ""));
	form->addRow(warp_flow_text("Warp.Flow.Trigger"), triggerCombo);

	orderCombo = new QComboBox(this);
	warp_fill_order_combo(orderCombo);
	warp_select_data(orderCombo, QString::fromUtf8(config ? obs_data_get_string(config, WARP_FLOW_ORDER) : ""));
	form->addRow(warp_flow_text("Warp.Flow.Order"), orderCombo);

	QWidget *limit_row = warp_limit_row(&limitCheck, &limitSpin, this);
	const int max_clips = config ? (int)obs_data_get_int(config, WARP_FLOW_MAX_CLIPS) : 0;

	limitCheck->setChecked(max_clips > 0);

	if (max_clips > 0)
		limitSpin->setValue(max_clips);

	form->addRow(warp_flow_text("Warp.Flow.Limit.Label"), limit_row);
	warp_set_row_visible(form, limit_row, !is_highlight);

	enabledCheck = new QCheckBox(warp_flow_text("Warp.Flow.Enabled"), this);
	enabledCheck->setChecked(config ? obs_data_get_bool(config, WARP_FLOW_ENABLED) : true);
	form->addRow(QString(), enabledCheck);

	/* the flows this one hands every clip it takes on to */
	linkList = new QListWidget(this);
	linkList->setIconSize(QSize(20, 20));

	QStringList linked;
	obs_data_array_t *links = config ? obs_data_get_array(config, WARP_FLOW_LINKS) : nullptr;
	const size_t link_count = links ? obs_data_array_count(links) : 0;

	for (size_t i = 0; i < link_count; i++) {
		obs_data_t *item = obs_data_array_item(links, i);

		linked.append(QString::fromUtf8(obs_data_get_string(item, WARP_FLOW_LINK_ID)));
		obs_data_release(item);
	}

	obs_data_array_release(links);

	obs_data_array_t *flows = warp_flow_list();
	const size_t flow_count = obs_data_array_count(flows);

	for (size_t i = 0; i < flow_count; i++) {
		obs_data_t *flow = obs_data_array_item(flows, i);
		const QString other_id = QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_ID));

		if (other_id != flowId) {
			auto *item = new QListWidgetItem(warp_flow_icon(obs_data_get_string(flow, WARP_FLOW_KIND)),
							 QString::fromUtf8(obs_data_get_string(flow, WARP_FLOW_NAME)),
							 linkList);

			item->setData(Qt::UserRole, other_id);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(linked.contains(other_id) ? Qt::Checked : Qt::Unchecked);
		}

		obs_data_release(flow);
	}

	obs_data_array_release(flows);

	auto *links_label = new QLabel(warp_flow_text("Warp.Flow.Links"), this);
	auto *links_desc = new QLabel(warp_flow_text("Warp.Flow.Links.Desc"), this);

	links_desc->setWordWrap(true);
	links_desc->setTextFormat(Qt::PlainText);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);

	layout->addLayout(header);
	layout->addWidget(warp_separator(this));
	layout->addLayout(form);
	layout->addWidget(links_label);
	layout->addWidget(links_desc);
	layout->addWidget(linkList, 1);
	layout->addWidget(buttons);

	connect(targetCombo, &QComboBox::currentIndexChanged, this, [this](int) { targetChanged(); });

	targetChanged();

	obs_data_release(config);
}

void WarpFlowPropsDialog::targetChanged()
{
	warp_set_row_visible(form, newTargetEdit, targetCombo->currentData().toString().isEmpty());
}

void WarpFlowPropsDialog::accept()
{
	const QString name = nameEdit->text().trimmed();

	if (name.isEmpty()) {
		QMessageBox::warning(this, warp_flow_text("Warp.Flow.Props.Title"),
				     warp_flow_text("Warp.Flow.Error.NameEmpty"));
		nameEdit->setFocus();
		return;
	}

	if (!warp_flow_name_free(this, name, flowId))
		return;

	QString uuid;
	QString target_name;

	if (!warp_resolve_target(this, targetCombo, newTargetEdit, name, uuid, target_name))
		return;

	obs_data_t *config = obs_data_create();

	obs_data_set_string(config, WARP_FLOW_NAME, name.toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_TARGET_UUID, uuid.toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_TARGET_NAME, target_name.toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_TRIGGER, triggerCombo->currentData().toString().toUtf8().constData());
	obs_data_set_string(config, WARP_FLOW_ORDER, orderCombo->currentData().toString().toUtf8().constData());
	obs_data_set_int(config, WARP_FLOW_MAX_CLIPS, limitCheck->isChecked() ? limitSpin->value() : 0);
	obs_data_set_bool(config, WARP_FLOW_ENABLED, enabledCheck->isChecked());

	obs_data_array_t *links = obs_data_array_create();

	for (int i = 0; i < linkList->count(); i++) {
		QListWidgetItem *item = linkList->item(i);

		if (item->checkState() != Qt::Checked)
			continue;

		obs_data_t *link = obs_data_create();

		obs_data_set_string(link, WARP_FLOW_LINK_ID, item->data(Qt::UserRole).toString().toUtf8().constData());
		obs_data_array_push_back(links, link);
		obs_data_release(link);
	}

	obs_data_set_array(config, WARP_FLOW_LINKS, links);
	obs_data_array_release(links);

	warp_flow_update(flowId.toUtf8().constData(), config);
	obs_data_release(config);

	QDialog::accept();
}
