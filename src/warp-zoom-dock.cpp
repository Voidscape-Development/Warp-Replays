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

#include <cmath>
#include <functional>

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <graphics/vec4.h>
#include <util/config-file.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QWheelEvent>

#include "warp-events.h"
#include "warp-flow.h"
#include "warp-zoom-dock.hpp"
#include "warp-zoom.h"

/* the same shorthand OBS Studio's own UI uses for handing a Qt string to a
 * libobs call; the bytes live until the end of the statement it is used in */
#define WARP_UTF8(str) (str).toUtf8().constData()

namespace {

/* How often the picture in the dock is redrawn, and how often the lists are
 * looked over for sources and presets that have come or gone.
 *
 * Grabbing the picture costs a render and a read back off the card, so the
 * resting rate is the one a dock that is being watched needs. While something
 * is being ridden - the zoom bar, or the picture under a drag - it is grabbed
 * twice as often, so a move is seen as it is made rather than a frame or two
 * after it. */
constexpr int WARP_ZOOM_PREVIEW_MS = 66;
constexpr int WARP_ZOOM_PREVIEW_RIDE_MS = 33;
constexpr int WARP_ZOOM_REFRESH_MS = 1000;

/* the longest side of the picture the dock grabs; a framing pad does not need
 * the full canvas, and this keeps the grab cheap */
constexpr int WARP_ZOOM_PREVIEW_MAX = 480;

/* how far one press of the dock's pan buttons moves, as a fraction of what is
 * on screen, matching the pan hotkeys */
constexpr float WARP_ZOOM_DOCK_PAN = WARP_ZOOM_PAN_STEP;

/* the speeds the dock offers as buttons, which are the ones the speed hotkeys
 * are bound to */
const int warpZoomSpeeds[] = {25, 50, 100, 150, 200};

/* Where the dock remembers how it was left. This is how an operator likes to
 * work rather than anything about the show, so it lives in the user's own
 * config instead of the scene collection. */
constexpr const char *WARP_ZOOM_CFG_SECTION = "WarpZoom";
constexpr const char *WARP_ZOOM_CFG_MINIMAL = "Minimal";

bool warpZoomMinimalSetting()
{
	config_t *cfg = obs_frontend_get_user_config();

	if (!cfg)
		return false;

	config_set_default_bool(cfg, WARP_ZOOM_CFG_SECTION, WARP_ZOOM_CFG_MINIMAL, false);

	return config_get_bool(cfg, WARP_ZOOM_CFG_SECTION, WARP_ZOOM_CFG_MINIMAL);
}

void warpZoomSetMinimalSetting(bool minimal)
{
	config_t *cfg = obs_frontend_get_user_config();

	if (!cfg)
		return;

	config_set_bool(cfg, WARP_ZOOM_CFG_SECTION, WARP_ZOOM_CFG_MINIMAL, minimal);
	config_save_safe(cfg, "tmp", nullptr);
}

/* ------------------------------------------------------------------------- */
/* the things that can be framed
 *
 * A target is a Warp source, which is zoomed by itself, or a Warp Zoom filter
 * an operator has put on any other source. Both are driven through the same
 * procs, so the dock holds them the same way: the source the picture comes
 * from, and the name of the filter when the zoom lives in one. */

struct WarpZoomTarget {
	QString label;
	QString parentUuid;
	QString filterName;

	bool isEmpty() const { return parentUuid.isEmpty(); }

	bool operator==(const WarpZoomTarget &other) const
	{
		return parentUuid == other.parentUuid && filterName == other.filterName;
	}

	bool operator!=(const WarpZoomTarget &other) const { return !(*this == other); }
};

/* the source the picture is taken from; the caller releases it */
obs_source_t *warpZoomParent(const WarpZoomTarget &target)
{
	if (target.parentUuid.isEmpty())
		return nullptr;

	return obs_get_source_by_uuid(WARP_UTF8(target.parentUuid));
}

/* what the framing is asked of - the source itself, or the filter on it - with
 * a reference the caller releases */
obs_source_t *warpZoomTargetSource(const WarpZoomTarget &target)
{
	obs_source_t *parent = warpZoomParent(target);

	if (!parent)
		return nullptr;

	if (target.filterName.isEmpty())
		return parent;

	obs_source_t *filter = obs_source_get_filter_by_name(parent, WARP_UTF8(target.filterName));

	obs_source_release(parent);

	return filter;
}

void warpZoomAddTarget(QVector<WarpZoomTarget> *targets, obs_source_t *source)
{
	const char *name = obs_source_get_name(source);
	const char *uuid = obs_source_get_uuid(source);

	if (!name || !*name || !uuid || !*uuid)
		return;

	if (warp_zoom_source_capable(source)) {
		WarpZoomTarget target;

		target.label = QString::fromUtf8(name);
		target.parentUuid = QString::fromUtf8(uuid);
		targets->append(target);
		return;
	}

	/* anything else is framed through the Warp Zoom filter on it, so a
	 * camera or a scene is in the list on the same terms as a playlist */
	obs_source_t *filter = warp_zoom_filter_find_operable(source);

	if (!filter)
		return;

	const char *filter_name = obs_source_get_name(filter);

	if (filter_name && *filter_name) {
		WarpZoomTarget target;

		target.label = QString("%1 - %2").arg(QString::fromUtf8(name), QString::fromUtf8(filter_name));
		target.parentUuid = QString::fromUtf8(uuid);
		target.filterName = QString::fromUtf8(filter_name);
		targets->append(target);
	}

	obs_source_release(filter);
}

bool warpZoomEnumTarget(void *data, obs_source_t *source)
{
	warpZoomAddTarget(static_cast<QVector<WarpZoomTarget> *>(data), source);
	return true;
}

QVector<WarpZoomTarget> warpZoomTargets()
{
	QVector<WarpZoomTarget> targets;

	obs_enum_sources(warpZoomEnumTarget, &targets);
	obs_enum_scenes(warpZoomEnumTarget, &targets);

	return targets;
}

/* what the dock follows when it is set to follow whatever is live: the first
 * thing in the scene on program that can be framed */
struct WarpZoomActiveSearch {
	WarpZoomTarget found;
	int depth;
};

bool warpZoomActiveItem(obs_scene_t *scene, obs_sceneitem_t *item, void *param);

void warpZoomSearchSource(WarpZoomActiveSearch *search, obs_source_t *source)
{
	QVector<WarpZoomTarget> targets;

	warpZoomAddTarget(&targets, source);

	if (!targets.isEmpty()) {
		search->found = targets.first();
		return;
	}

	obs_scene_t *nested = obs_scene_from_source(source);

	/* a replay feed is often a scene of its own inside the one on air, so
	 * the search goes in a little way rather than only skimming the top */
	if (nested && search->depth < 3) {
		search->depth++;
		obs_scene_enum_items(nested, warpZoomActiveItem, search);
		search->depth--;
	}
}

bool warpZoomActiveItem(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	WarpZoomActiveSearch *search = static_cast<WarpZoomActiveSearch *>(param);

	UNUSED_PARAMETER(scene);

	if (!search->found.isEmpty())
		return false;

	if (!obs_sceneitem_visible(item))
		return true;

	warpZoomSearchSource(search, obs_sceneitem_get_source(item));

	return search->found.isEmpty();
}

WarpZoomTarget warpZoomActiveTarget()
{
	WarpZoomActiveSearch search;
	obs_source_t *scene_source = obs_frontend_get_current_scene();

	search.depth = 0;

	if (!scene_source)
		return search.found;

	obs_scene_t *scene = obs_scene_from_source(scene_source);

	if (scene)
		obs_scene_enum_items(scene, warpZoomActiveItem, &search);

	obs_source_release(scene_source);

	return search.found;
}

/* ------------------------------------------------------------------------- */
/* the picture
 *
 * The dock draws what the source is putting out, framed as it is now, and pans
 * by dragging it: the operator moves the picture with the cursor the way they
 * would move a camera, rather than typing numbers at it. */

class WarpZoomPreview : public QWidget {
public:
	explicit WarpZoomPreview(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumHeight(180);
		setCursor(Qt::OpenHandCursor);
		setMouseTracking(false);
	}

	/* Called as the picture is dragged, in fractions of what is on screen,
	 * and as the wheel is turned, as what to multiply the zoom by. */
	std::function<void(float, float)> onPan;
	std::function<void(float)> onZoom;
	/* whether the picture is being held, so the dock can keep up with a
	 * drag while there is one */
	std::function<void(bool)> onHold;

	void setImage(const QImage &image)
	{
		picture = image;
		update();
	}

	void setView(const warp_zoom_view &view)
	{
		framing = view;
		update();
	}

	/* The shot being lined up, which is what the picture shows while there
	 * is one: an operator judges a framing by looking at it, not at a box. */
	void setStage(const warp_zoom_view &view, bool is_staged)
	{
		stage = view;
		staged = is_staged;
		update();
	}

	void setLive(bool value)
	{
		if (live == value)
			return;

		live = value;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		QRect area = rect();

		painter.fillRect(area, QColor(20, 20, 20));

		if (picture.isNull()) {
			painter.setPen(QColor(160, 160, 160));
			painter.drawText(area, Qt::AlignCenter,
					 QString::fromUtf8(obs_module_text(live ? "Warp.Zoom.Dock.NoPicture"
										: "Warp.Zoom.Dock.NoTarget")));
			return;
		}

		QSize scaled = picture.size();

		scaled.scale(area.size(), Qt::KeepAspectRatio);
		shown = QRect(QPoint(area.x() + (area.width() - scaled.width()) / 2,
				     area.y() + (area.height() - scaled.height()) / 2),
			      scaled);

		if (staged)
			drawStaged(painter);
		else
			painter.drawImage(shown, picture);

		drawMinimap(painter);
		drawBadge(painter);
	}

	/* Draws the staged shot out of the picture that is on air.
	 *
	 * The only pixels anyone has are the ones the source is putting out,
	 * framed as it is now, so a shot lined up inside that - which is what
	 * tightening in from a wider shot always is - is exact. A shot that
	 * reaches outside it cannot be drawn from what we have, and those parts
	 * are left as hatching rather than invented. */
	void drawStaged(QPainter &painter)
	{
		float live_window = 1.0f / framing.zoom;
		float stage_window = 1.0f / stage.zoom;

		/* the staged window, in fractions of the picture on screen */
		double u0 = ((stage.x - stage_window * 0.5f) - (framing.x - live_window * 0.5f)) / live_window;
		double v0 = ((stage.y - stage_window * 0.5f) - (framing.y - live_window * 0.5f)) / live_window;
		double du = stage_window / live_window;

		QRectF source(u0 * picture.width(), v0 * picture.height(), du * picture.width(), du * picture.height());
		QRectF have = source.intersected(QRectF(0, 0, picture.width(), picture.height()));

		painter.fillRect(shown, QBrush(QColor(70, 70, 74), Qt::BDiagPattern));

		if (have.isEmpty() || source.width() <= 0.0 || source.height() <= 0.0)
			return;

		/* whatever part of the shot we do have goes in the matching part
		 * of the frame, so it stays where it belongs in the composition */
		QRectF target(shown.x() + (have.left() - source.left()) / source.width() * shown.width(),
			      shown.y() + (have.top() - source.top()) / source.height() * shown.height(),
			      have.width() / source.width() * shown.width(),
			      have.height() / source.height() * shown.height());

		painter.drawImage(target, picture, have);

		painter.setPen(QPen(QColor(224, 152, 44), 2));
		painter.drawRect(QRectF(shown).adjusted(1, 1, -1, -1));
	}

	void drawBadge(QPainter &painter)
	{
		QRect box = shown.adjusted(6, 4, -6, -4);

		painter.setPen(QColor(255, 255, 255));
		painter.drawText(box, Qt::AlignLeft | Qt::AlignTop, QString("%1%").arg(qRound(framing.zoom * 100.0f)));

		if (!staged)
			return;

		painter.setPen(QColor(224, 152, 44));
		painter.drawText(
			box, Qt::AlignRight | Qt::AlignTop,
			QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Staged")).arg(qRound(stage.zoom * 100.0f)));
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton)
			return;

		dragging = true;
		last = event->position();
		setCursor(Qt::ClosedHandCursor);

		if (onHold)
			onHold(true);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!dragging || shown.isEmpty())
			return;

		QPointF now = event->position();
		QPointF delta = now - last;

		last = now;

		if (!onPan)
			return;

		/* The picture follows the cursor: moving the mouse right moves
		 * what is shown right, which means the window it is taken from
		 * moves left. A drag is direct handling rather than a nudge, so
		 * it is not eased - the picture stays under the finger.
		 *
		 * While a shot is being lined up the drag moves that one
		 * instead. Either way the picture on screen is the window being
		 * moved, and panning works in fractions of that window, so the
		 * same fraction of the widget is the right amount to ask for. */
		onPan(-(float)(delta.x() / shown.width()), -(float)(delta.y() / shown.height()));
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		UNUSED_PARAMETER(event);

		if (dragging && onHold)
			onHold(false);

		dragging = false;
		setCursor(Qt::OpenHandCursor);
	}

	void wheelEvent(QWheelEvent *event) override
	{
		int steps = event->angleDelta().y();

		if (!steps || !onZoom)
			return;

		onZoom(powf(WARP_ZOOM_STEP, (float)steps / 120.0f));
		event->accept();
	}

private:
	/* Where the framing sits in the whole picture, drawn in the corner: the
	 * dock shows what is on air rather than the whole file, so this is what
	 * says how much is being left out and which way. */
	void drawMinimap(QPainter &painter)
	{
		if (framing.zoom <= WARP_ZOOM_MIN && !staged)
			return;

		const int size = 56;
		QRect box(shown.right() - size - 8, shown.bottom() - (size * 9 / 16) - 8, size, size * 9 / 16);

		painter.setPen(QColor(255, 255, 255, 140));
		painter.setBrush(QColor(0, 0, 0, 110));
		painter.drawRect(box);

		/* what is on air, and the shot waiting behind it: with a stage up
		 * the two boxes are what say where the move is going */
		drawWindow(painter, box, framing, QColor(255, 255, 255, 150), QColor(255, 255, 255, 60));

		if (staged)
			drawWindow(painter, box, stage, QColor(224, 152, 44), QColor(224, 152, 44, 70));
	}

	void drawWindow(QPainter &painter, const QRect &box, const warp_zoom_view &view, const QColor &pen,
			const QColor &fill)
	{
		float window = 1.0f / view.zoom;
		QRectF inner(box.x() + (view.x - window * 0.5f) * box.width(),
			     box.y() + (view.y - window * 0.5f) * box.height(), window * box.width(),
			     window * box.height());

		painter.setPen(pen);
		painter.setBrush(fill);
		painter.drawRect(inner);
	}

	QImage picture;
	QRect shown;
	QPointF last;
	warp_zoom_view framing = warp_zoom_default_view();
	warp_zoom_view stage = warp_zoom_default_view();
	bool staged = false;
	bool dragging = false;
	bool live = false;
};

/* ------------------------------------------------------------------------- */
/* grabbing the picture */

class WarpZoomGrabber {
public:
	~WarpZoomGrabber() { release(); }

	/* What the source is putting out right now, at something like preview
	 * size. Rendering a source outside the main loop means holding the
	 * graphics context, so the whole grab is done in one pass and the
	 * surfaces it needs are kept between frames. */
	QImage grab(obs_source_t *source)
	{
		uint32_t source_cx = obs_source_get_width(source);
		uint32_t source_cy = obs_source_get_height(source);

		if (!source_cx || !source_cy)
			return QImage();

		uint32_t grab_cx = source_cx;
		uint32_t grab_cy = source_cy;

		if (grab_cx > WARP_ZOOM_PREVIEW_MAX) {
			grab_cy = grab_cy * WARP_ZOOM_PREVIEW_MAX / grab_cx;
			grab_cx = WARP_ZOOM_PREVIEW_MAX;
		}

		if (!grab_cx || !grab_cy)
			return QImage();

		QImage image;

		/* An async source only uploads its frames while it is being
		 * shown, so a source that is not on air anywhere would be
		 * grabbed as nothing at all. Saying so walks libobs' source
		 * tree, which is done either side of the graphics context
		 * rather than from inside it. */
		obs_source_inc_showing(source);

		obs_enter_graphics();

		if (grab_cx != cx || grab_cy != cy) {
			releaseLocked();
			cx = grab_cx;
			cy = grab_cy;
		}

		if (!texrender)
			texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!stage)
			stage = gs_stagesurface_create(cx, cy, GS_RGBA);

		gs_texrender_reset(texrender);

		if (texrender && stage && gs_texrender_begin(texrender, cx, cy)) {
			struct vec4 clear;

			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)source_cx, 0.0f, (float)source_cy, -100.0f, 100.0f);

			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

			obs_source_video_render(source);

			gs_blend_state_pop();
			gs_texrender_end(texrender);

			gs_stage_texture(stage, gs_texrender_get_texture(texrender));

			uint8_t *data = nullptr;
			uint32_t linesize = 0;

			if (gs_stagesurface_map(stage, &data, &linesize)) {
				image = QImage(data, (int)cx, (int)cy, (int)linesize, QImage::Format_RGBA8888).copy();
				gs_stagesurface_unmap(stage);
			}
		}

		obs_leave_graphics();

		obs_source_dec_showing(source);

		return image;
	}

	void release()
	{
		if (!texrender && !stage)
			return;

		obs_enter_graphics();
		releaseLocked();
		obs_leave_graphics();
	}

private:
	void releaseLocked()
	{
		if (texrender) {
			gs_texrender_destroy(texrender);
			texrender = nullptr;
		}

		if (stage) {
			gs_stagesurface_destroy(stage);
			stage = nullptr;
		}
	}

	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stage = nullptr;
	uint32_t cx = 0;
	uint32_t cy = 0;
};

/* ------------------------------------------------------------------------- */
/* presets
 *
 * The presets of a source are read back through its procs as the array they are
 * saved as, so the dock and the presets window work the same way whether they
 * are holding a playlist, a media source or a filter on a camera. */

struct WarpZoomPresetRow {
	QString id;
	QString name;
	warp_zoom_view view;
	int glide;
	bool fixed;
};

QVector<WarpZoomPresetRow> warpZoomReadPresets(obs_source_t *source)
{
	QVector<WarpZoomPresetRow> rows;
	obs_data_array_t *array = source ? warp_zoom_source_presets(source) : nullptr;

	if (!array)
		return rows;

	for (size_t i = 0; i < obs_data_array_count(array); i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		WarpZoomPresetRow row;

		row.id = QString::fromUtf8(obs_data_get_string(item, WARP_ZOOM_P_ID));
		row.name = QString::fromUtf8(obs_data_get_string(item, WARP_ZOOM_P_NAME));
		row.view.zoom = (float)obs_data_get_double(item, WARP_ZOOM_P_ZOOM);
		row.view.x = (float)obs_data_get_double(item, WARP_ZOOM_P_X);
		row.view.y = (float)obs_data_get_double(item, WARP_ZOOM_P_Y);
		row.glide = (int)obs_data_get_int(item, WARP_ZOOM_P_GLIDE);
		row.fixed = obs_data_get_bool(item, WARP_ZOOM_P_FIXED);

		rows.append(row);
		obs_data_release(item);
	}

	obs_data_array_release(array);

	return rows;
}

/* How a preset reads in a list: the reset position says as much, and the ones
 * after it carry the number of the slot hotkey that fires them. */
QString warpZoomPresetLabel(const WarpZoomPresetRow &row, int index)
{
	if (row.fixed)
		return QString("%1 - %2").arg(row.name, QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Reset")));

	if (index >= 1 && index <= WARP_ZOOM_SLOTS)
		return QString("%1.  %2  (%3%)").arg(index).arg(row.name).arg(qRound(row.view.zoom * 100.0f));

	return QString("%1  (%2%)").arg(row.name).arg(qRound(row.view.zoom * 100.0f));
}

/* asks the source to save what it is framed with, under a name the operator
 * types, the way a PTZ desk stores a shot */
void warpZoomSavePreset(QWidget *parent, obs_source_t *source)
{
	bool ok = false;
	QString name = QInputDialog::getText(parent, QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Save.Title")),
					     QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Save.Prompt")),
					     QLineEdit::Normal, QString(), &ok);

	if (!ok)
		return;

	char *id = warp_zoom_source_save_preset(source, WARP_UTF8(name.trimmed()));

	if (!id) {
		QMessageBox::warning(parent, QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Save.Title")),
				     QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Save.Failed")));
		return;
	}

	bfree(id);
}

/* ------------------------------------------------------------------------- */
/* the dock */

class WarpZoomDock : public QWidget {
public:
	explicit WarpZoomDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		build();

		previewTimer = new QTimer(this);
		connect(previewTimer, &QTimer::timeout, this, [this]() { refreshPicture(); });
		previewTimer->start(WARP_ZOOM_PREVIEW_MS);

		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, [this]() { refreshLists(); });
		refreshTimer->start(WARP_ZOOM_REFRESH_MS);

		setMinimal(warpZoomMinimalSetting());
		refreshLists();
	}

	~WarpZoomDock() override { grabber.release(); }

protected:
	/* Full or Minimal, on the dock itself: a setting you choose once does
	 * not deserve a control taking up room in a panel meant to be small. */
	void contextMenuEvent(QContextMenuEvent *event) override
	{
		QMenu menu(this);
		QAction *full = menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Mode.Full")));
		QAction *small = menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Mode.Minimal")));

		full->setCheckable(true);
		small->setCheckable(true);
		full->setChecked(!minimal);
		small->setChecked(minimal);

		QAction *picked = menu.exec(event->globalPos());

		if (!picked)
			return;

		bool wanted = picked == small;

		if (wanted == minimal)
			return;

		setMinimal(wanted);
		warpZoomSetMinimalSetting(minimal);
	}

private:
	/* Minimal drops the picture and nothing else: the pad, the presets and
	 * the speed are what an operator works, and without the picture there
	 * is no per-frame grab to pay for either. */
	void setMinimal(bool value)
	{
		minimal = value;
		preview->setVisible(!minimal);
	}

	void build()
	{
		auto *layout = new QVBoxLayout(this);

		/* which source is being framed */
		targets = new QComboBox(this);
		targets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

		follow = new QCheckBox(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Follow")), this);
		follow->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Follow.Desc")));

		auto *targetRow = new QHBoxLayout();

		targetRow->addWidget(targets, 1);
		targetRow->addWidget(follow);

		preview = new WarpZoomPreview(this);
		preview->onPan = [this](float dx, float dy) {
			pan(dx, dy, 0);
		};
		preview->onZoom = [this](float factor) {
			adjust(factor, -1);
		};
		preview->onHold = [this](bool holding) {
			setRiding(holding);
		};

		/* zoom */
		zoomSlider = new QSlider(Qt::Horizontal, this);
		zoomSlider->setRange((int)(WARP_ZOOM_MIN * 100), (int)(WARP_ZOOM_MAX * 100));
		zoomSlider->setValue((int)(WARP_ZOOM_MIN * 100));

		zoomLabel = new QLabel(this);
		zoomLabel->setMinimumWidth(48);
		zoomLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		auto *zoomRow = new QHBoxLayout();

		zoomRow->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Warp.Zoom.Zoom")), this));
		zoomRow->addWidget(zoomSlider, 1);
		zoomRow->addWidget(zoomLabel);

		/* The pad: eight directions around a reset, with the zoom either
		 * side, laid out the way a PTZ desk is. A corner moves a full
		 * step on both axes, the way a joystick held into its corner
		 * covers more ground than one pushed straight. */
		auto *pad = new QGridLayout();
		auto *up = new QPushButton(QString::fromUtf8("▲"), this);
		auto *down = new QPushButton(QString::fromUtf8("▼"), this);
		auto *left = new QPushButton(QString::fromUtf8("◀"), this);
		auto *right = new QPushButton(QString::fromUtf8("▶"), this);
		auto *upLeft = new QPushButton(QString::fromUtf8("◤"), this);
		auto *upRight = new QPushButton(QString::fromUtf8("◥"), this);
		auto *downLeft = new QPushButton(QString::fromUtf8("◣"), this);
		auto *downRight = new QPushButton(QString::fromUtf8("◢"), this);
		auto *in = new QPushButton(QString::fromUtf8("+"), this);
		auto *out = new QPushButton(QString::fromUtf8("−"), this);

		resetButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Reset")), this);
		resetButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Reset.Desc")));

		pad->addWidget(upLeft, 0, 1);
		pad->addWidget(up, 0, 2);
		pad->addWidget(upRight, 0, 3);
		pad->addWidget(out, 1, 0);
		pad->addWidget(left, 1, 1);
		pad->addWidget(resetButton, 1, 2);
		pad->addWidget(right, 1, 3);
		pad->addWidget(in, 1, 4);
		pad->addWidget(downLeft, 2, 1);
		pad->addWidget(down, 2, 2);
		pad->addWidget(downRight, 2, 3);

		connect(up, &QPushButton::clicked, this, [this]() { pan(0.0f, -WARP_ZOOM_DOCK_PAN, -1); });
		connect(down, &QPushButton::clicked, this, [this]() { pan(0.0f, WARP_ZOOM_DOCK_PAN, -1); });
		connect(left, &QPushButton::clicked, this, [this]() { pan(-WARP_ZOOM_DOCK_PAN, 0.0f, -1); });
		connect(right, &QPushButton::clicked, this, [this]() { pan(WARP_ZOOM_DOCK_PAN, 0.0f, -1); });
		connect(upLeft, &QPushButton::clicked, this,
			[this]() { pan(-WARP_ZOOM_DOCK_PAN, -WARP_ZOOM_DOCK_PAN, -1); });
		connect(upRight, &QPushButton::clicked, this,
			[this]() { pan(WARP_ZOOM_DOCK_PAN, -WARP_ZOOM_DOCK_PAN, -1); });
		connect(downLeft, &QPushButton::clicked, this,
			[this]() { pan(-WARP_ZOOM_DOCK_PAN, WARP_ZOOM_DOCK_PAN, -1); });
		connect(downRight, &QPushButton::clicked, this,
			[this]() { pan(WARP_ZOOM_DOCK_PAN, WARP_ZOOM_DOCK_PAN, -1); });
		connect(in, &QPushButton::clicked, this, [this]() { adjust(WARP_ZOOM_STEP, -1); });
		connect(out, &QPushButton::clicked, this, [this]() { adjust(1.0f / WARP_ZOOM_STEP, -1); });
		connect(resetButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();

			if (source)
				warp_zoom_source_reset(source, -1);
		});

		/* Lining a shot up before it goes to air. The take and drop
		 * buttons are only there while something is waiting, so the row
		 * carries nothing dead. */
		confirm = new QCheckBox(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Confirm")), this);
		confirm->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Confirm.Desc")));

		takeButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Take")), this);
		dropButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Drop")), this);

		takeButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Take.Desc")));
		takeButton->setVisible(false);
		dropButton->setVisible(false);

		auto *confirmRow = new QHBoxLayout();

		confirmRow->addWidget(confirm, 1);
		confirmRow->addWidget(takeButton);
		confirmRow->addWidget(dropButton);

		connect(confirm, &QCheckBox::toggled, this, [this](bool on) {
			OBSSourceAutoRelease source = zoomSource();

			if (loading || !source)
				return;

			warp_zoom_source_set_confirm(source, on);
			refreshView();
		});

		connect(takeButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();

			if (source)
				warp_zoom_source_take(source);

			refreshView();
		});

		connect(dropButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();

			if (source)
				warp_zoom_source_drop(source);

			refreshView();
		});

		/* presets */
		presets = new QListWidget(this);
		presets->setSelectionMode(QAbstractItemView::SingleSelection);

		auto *saveButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Save")), this);
		auto *recallButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Recall")), this);

		updateButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Update")), this);
		renameButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Rename")), this);
		removeButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Remove")), this);

		saveButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Save.Desc")));
		updateButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Update.Desc")));

		auto *presetButtons = new QHBoxLayout();

		presetButtons->addWidget(recallButton);
		presetButtons->addWidget(saveButton);
		presetButtons->addWidget(updateButton);
		presetButtons->addWidget(renameButton);
		presetButtons->addWidget(removeButton);
		presetButtons->addStretch(1);

		connect(recallButton, &QPushButton::clicked, this, [this]() { recall(presets->currentItem()); });

		/* Recalling is a double-click or the button, never a single click:
		 * picking a preset to rename must not put it on air. */
		connect(presets, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { recall(item); });
		connect(presets, &QListWidget::itemSelectionChanged, this, [this]() { updateButtons(); });

		connect(saveButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();

			if (!source)
				return;

			warpZoomSavePreset(this, source);
			refreshLists();
		});

		connect(updateButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();
			QString id = selectedPreset();

			if (!source || id.isEmpty())
				return;

			warp_zoom_view view;

			/* Whatever is being looked at: the shot lined up when
			 * there is one, and otherwise the framing it is heading
			 * for, so keeping a shot partway through a move keeps
			 * the shot rather than wherever the move had got to. */
			if (framingNow(source, &view))
				warp_zoom_source_update_preset(source, WARP_UTF8(id), nullptr, &view, -1);

			refreshLists();
		});

		connect(renameButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();
			QString id = selectedPreset();
			bool ok = false;

			if (!source || id.isEmpty())
				return;

			QString name = QInputDialog::getText(
				this, QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Rename")),
				QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Save.Prompt")), QLineEdit::Normal,
				selectedName(), &ok);

			if (ok && !name.trimmed().isEmpty())
				warp_zoom_source_update_preset(source, WARP_UTF8(id), WARP_UTF8(name.trimmed()),
							       nullptr, -1);

			refreshLists();
		});

		connect(removeButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();
			QString id = selectedPreset();

			if (!source || id.isEmpty())
				return;

			warp_zoom_source_remove_preset(source, WARP_UTF8(id));
			refreshLists();
		});

		/* speed, which is the other thing an operator rides live */
		speedSlider = new QSlider(Qt::Horizontal, this);
		speedSlider->setRange(WARP_FLOW_SPEED_MIN, WARP_FLOW_SPEED_MAX);
		speedSlider->setValue(100);

		speedLabel = new QLabel(this);
		speedLabel->setMinimumWidth(48);
		speedLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		auto *speedRow = new QHBoxLayout();

		speedRow->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Warp.Video.Speed")), this));
		speedRow->addWidget(speedSlider, 1);
		speedRow->addWidget(speedLabel);

		auto *speedButtons = new QHBoxLayout();

		for (int speed : warpZoomSpeeds) {
			auto *button = new QPushButton(QString("%1%").arg(speed), this);

			connect(button, &QPushButton::clicked, this, [this, speed]() { setSpeed(speed); });
			speedButtons->addWidget(button);
		}

		speedButtons->addStretch(1);

		connect(targets, &QComboBox::currentIndexChanged, this, [this](int index) {
			if (loading || index < 0 || index >= chosen.size())
				return;

			target = chosen[index];
			refreshLists();
		});

		connect(follow, &QCheckBox::toggled, this, [this](bool) { refreshLists(); });

		connect(zoomSlider, &QSlider::valueChanged, this, [this](int value) {
			if (loading)
				return;

			OBSSourceAutoRelease source = zoomSource();

			if (!source)
				return;

			warp_zoom_view view;
			bool staging = false;

			if (!framingNow(source, &view, &staging))
				return;

			view.zoom = (float)value / 100.0f;

			/* Riding the bar is direct handling, the same as dragging
			 * the picture: where the handle is put is where the
			 * picture is, with nothing eased behind it. Naming no
			 * glide - which is what this used to do - asks for the
			 * source's preset glide instead, so every step of a drag
			 * started a fresh move of most of a second and the
			 * picture crawled along behind the handle without ever
			 * catching it. A step from a click on the groove or from
			 * the arrow keys is one move rather than a run of them,
			 * so it is nudged the way the pad's presses are. */
			warp_zoom_source_set(source, &view, zoomSlider->isSliderDown() ? 0 : WARP_ZOOM_NUDGE_MS);

			warp_zoom_clamp(&view);
			showFraming(view, staging);
		});

		connect(zoomSlider, &QSlider::sliderPressed, this, [this]() { setRiding(true); });
		connect(zoomSlider, &QSlider::sliderReleased, this, [this]() { setRiding(false); });

		connect(speedSlider, &QSlider::valueChanged, this, [this](int value) {
			if (!loading)
				setSpeed(value);
		});

		layout->addLayout(targetRow);
		layout->addWidget(preview, 1);
		layout->addLayout(zoomRow);
		layout->addLayout(pad);
		layout->addLayout(confirmRow);
		layout->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Presets")), this));
		layout->addWidget(presets, 1);
		layout->addLayout(presetButtons);
		layout->addLayout(speedRow);
		layout->addLayout(speedButtons);
	}

	obs_source_t *zoomSource() const { return warpZoomTargetSource(target); }

	/* The picture is grabbed twice as often while a control is being ridden,
	 * so what the operator is doing is seen as they do it rather than at the
	 * resting rate. */
	void setRiding(bool riding)
	{
		int wanted = riding ? WARP_ZOOM_PREVIEW_RIDE_MS : WARP_ZOOM_PREVIEW_MS;

		if (previewTimer && previewTimer->interval() != wanted)
			previewTimer->setInterval(wanted);
	}

	/* The framing the operator is looking at: the staged shot while one is
	 * being lined up, and where the picture is heading otherwise.
	 * 'staging' answers whether a change made to it would be lined up
	 * rather than go to air, which is what confirm mode says and is not the
	 * same question as whether a shot is waiting already. */
	static bool framingNow(obs_source_t *source, warp_zoom_view *out, bool *staging = nullptr)
	{
		bool is_staged = false;
		bool confirming = false;
		bool have = warp_zoom_source_stage(source, out, &is_staged, &confirming);

		if (staging)
			*staging = have && confirming;

		if (have && is_staged)
			return true;

		return warp_zoom_source_get(source, nullptr, out);
	}

	/* What the readout says the framing is, without waiting for the next
	 * tick of the preview timer. A control that answers fifteen times a
	 * second reads as one that is lagging behind the hand on it, and saying
	 * so costs a label and a repaint rather than another grab. */
	void showFraming(const warp_zoom_view &view, bool staging)
	{
		zoomLabel->setText(QString("%1%").arg(qRound(view.zoom * 100.0f)));

		/* in confirm mode nothing has moved on screen, so what is drawn
		 * is the shot being lined up rather than the picture */
		if (staging)
			preview->setStage(view, true);
		else
			preview->setView(view);
	}

	QString selectedPreset() const
	{
		QListWidgetItem *item = presets->currentItem();

		return item ? item->data(Qt::UserRole).toString() : QString();
	}

	QString selectedName() const
	{
		QListWidgetItem *item = presets->currentItem();

		return item ? item->data(Qt::UserRole + 1).toString() : QString();
	}

	void recall(QListWidgetItem *item)
	{
		OBSSourceAutoRelease source = zoomSource();

		if (source && item)
			warp_zoom_source_recall(source, WARP_UTF8(item->data(Qt::UserRole).toString()));

		updateButtons();
	}

	void pan(float dx, float dy, int glide)
	{
		OBSSourceAutoRelease source = zoomSource();

		if (source)
			warp_zoom_source_pan(source, dx, dy, glide);
	}

	void adjust(float factor, int glide)
	{
		OBSSourceAutoRelease source = zoomSource();

		if (source)
			warp_zoom_source_adjust(source, factor, glide);
	}

	void setSpeed(int speed)
	{
		OBSSourceAutoRelease parent = warpZoomParent(target);
		calldata_t cd;

		if (!parent)
			return;

		calldata_init(&cd);
		calldata_set_int(&cd, "speed", speed);
		proc_handler_call(obs_source_get_proc_handler(parent), "warp_set_speed", &cd);
		calldata_free(&cd);
	}

	void updateButtons()
	{
		QListWidgetItem *item = presets->currentItem();
		bool editable = item && !item->data(Qt::UserRole + 2).toBool();

		/* the reset position is always there and always the whole
		 * picture: it is recalled like any other preset and changed
		 * like none of them */
		updateButton->setEnabled(editable);
		renameButton->setEnabled(editable);
		removeButton->setEnabled(editable);
	}

	/* the sources that can be framed, and the presets of the one that is */
	void refreshLists()
	{
		QVector<WarpZoomTarget> found = warpZoomTargets();

		if (follow->isChecked()) {
			WarpZoomTarget live = warpZoomActiveTarget();

			if (!live.isEmpty())
				target = live;
		}

		loading = true;

		if (found != chosen) {
			chosen = found;
			targets->clear();

			for (const WarpZoomTarget &entry : chosen)
				targets->addItem(entry.label);
		}

		int index = chosen.indexOf(target);

		/* nothing picked yet, or what was picked has gone: the first
		 * thing that can be framed is better than an empty dock */
		if (index < 0 && !chosen.isEmpty()) {
			target = chosen.first();
			index = 0;
		}

		/* A shot lined up on the source the dock has just left goes with
		 * it: nobody is looking at it any more, and one left waiting is
		 * a hazard the next time somebody presses take. */
		if (!held.isEmpty() && held != target) {
			OBSSourceAutoRelease previous = warpZoomTargetSource(held);

			if (previous)
				warp_zoom_source_drop(previous);
		}

		held = target;

		targets->setCurrentIndex(index);
		targets->setEnabled(!follow->isChecked());

		loading = false;

		refreshPresets();
		refreshView();
	}

	void refreshPresets()
	{
		OBSSourceAutoRelease source = zoomSource();
		QVector<WarpZoomPresetRow> rows = warpZoomReadPresets(source);
		QString selected = selectedPreset();

		QStringList labels;

		for (int i = 0; i < rows.size(); i++)
			labels << warpZoomPresetLabel(rows[i], i);

		/* the list is rebuilt only when it has actually changed, so a
		 * preset stays selected while the dock ticks over */
		if (labels == shownPresets)
			return;

		shownPresets = labels;
		presets->clear();

		for (int i = 0; i < rows.size(); i++) {
			auto *item = new QListWidgetItem(labels[i], presets);

			item->setData(Qt::UserRole, rows[i].id);
			item->setData(Qt::UserRole + 1, rows[i].name);
			item->setData(Qt::UserRole + 2, rows[i].fixed);

			if (rows[i].id == selected)
				presets->setCurrentItem(item);
		}

		updateButtons();
	}

	void refreshView()
	{
		OBSSourceAutoRelease source = zoomSource();
		warp_zoom_view view = warp_zoom_default_view();
		warp_zoom_view target = warp_zoom_default_view();
		warp_zoom_view stage = warp_zoom_default_view();
		bool have = source && warp_zoom_source_get(source, &view, &target);
		bool is_staged = false;
		bool confirming = false;

		if (have)
			warp_zoom_source_stage(source, &stage, &is_staged, &confirming);

		loading = true;

		zoomSlider->setEnabled(have);

		/* The slider and the number follow the shot being lined up when
		 * there is one, since that is the one being worked on, and
		 * otherwise what the picture was asked for rather than where the
		 * move has got to: a control says what it has been set to. Read
		 * from a move in flight instead, the handle would crawl to its
		 * own destination behind the operator's hand, and a step taken
		 * with the keys would be pulled back the moment it was made. The
		 * picture itself shows the move, in the badge and the minimap. */
		const warp_zoom_view &shown = is_staged ? stage : target;

		if (!zoomSlider->isSliderDown())
			zoomSlider->setValue((int)(shown.zoom * 100.0f));

		zoomLabel->setText(QString("%1%").arg(qRound(shown.zoom * 100.0f)));

		confirm->setEnabled(have);
		confirm->setChecked(confirming);
		takeButton->setVisible(is_staged);
		dropButton->setVisible(is_staged);

		preview->setStage(stage, is_staged);

		OBSSourceAutoRelease parent = warpZoomParent(target);
		calldata_t cd;
		long long speed = 0;

		calldata_init(&cd);

		bool have_speed = parent &&
				  proc_handler_call(obs_source_get_proc_handler(parent), "warp_get_speed", &cd) &&
				  calldata_get_int(&cd, "speed", &speed);

		calldata_free(&cd);

		/* the speed controls are for the Warp sources; a camera with a
		 * zoom filter on it has no playback to ride */
		speedSlider->setEnabled(have_speed);

		if (have_speed && speed >= WARP_FLOW_SPEED_MIN) {
			if (!speedSlider->isSliderDown())
				speedSlider->setValue((int)speed);

			speedLabel->setText(QString("%1%").arg((int)speed));
		} else {
			speedLabel->setText(QString());
		}

		loading = false;

		preview->setView(view);
	}

	void refreshPicture()
	{
		if (!isVisible())
			return;

		if (minimal) {
			/* nothing to draw, so nothing is rendered: the dock
			 * costs only what its widgets cost while it is minimal */
			refreshView();
			return;
		}

		OBSSourceAutoRelease parent = warpZoomParent(target);

		preview->setLive(parent != nullptr);
		preview->setImage(parent ? grabber.grab(parent) : QImage());

		refreshView();
	}

	QComboBox *targets = nullptr;
	QCheckBox *follow = nullptr;
	QCheckBox *confirm = nullptr;
	QPushButton *takeButton = nullptr;
	QPushButton *dropButton = nullptr;
	WarpZoomPreview *preview = nullptr;
	QSlider *zoomSlider = nullptr;
	QLabel *zoomLabel = nullptr;
	QPushButton *resetButton = nullptr;
	QPushButton *updateButton = nullptr;
	QPushButton *renameButton = nullptr;
	QPushButton *removeButton = nullptr;
	QListWidget *presets = nullptr;
	QSlider *speedSlider = nullptr;
	QLabel *speedLabel = nullptr;
	QTimer *previewTimer = nullptr;
	QTimer *refreshTimer = nullptr;

	QVector<WarpZoomTarget> chosen;
	QStringList shownPresets;
	WarpZoomTarget target;
	/* what the dock was holding last time round, so leaving a source can
	 * drop the shot it had waiting */
	WarpZoomTarget held;
	WarpZoomGrabber grabber;
	bool loading = false;
	bool minimal = false;
};

/* ------------------------------------------------------------------------- */
/* the presets window
 *
 * What the dock does by eye, this does by number: the framing of every preset
 * of a source, and how long the move to it takes, written out so a show can be
 * set up before there is anything to look at. */

class WarpZoomPresetsDialog : public QDialog {
public:
	explicit WarpZoomPresetsDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(QString::fromUtf8(obs_module_text("Warp.Zoom.Presets.Title")));
		setMinimumSize(520, 420);

		auto *layout = new QVBoxLayout(this);

		sources = new QComboBox(this);
		chosen = warpZoomTargets();

		for (const WarpZoomTarget &entry : chosen)
			sources->addItem(entry.label);

		presets = new QListWidget(this);

		zoom = new QDoubleSpinBox(this);
		zoom->setRange(WARP_ZOOM_MIN * 100.0, WARP_ZOOM_MAX * 100.0);
		zoom->setSuffix(QString::fromUtf8("%"));

		x = new QDoubleSpinBox(this);
		x->setRange(0.0, 100.0);
		x->setSuffix(QString::fromUtf8("%"));

		y = new QDoubleSpinBox(this);
		y->setRange(0.0, 100.0);
		y->setSuffix(QString::fromUtf8("%"));

		glide = new QSpinBox(this);
		glide->setRange(0, WARP_ZOOM_GLIDE_MAX);
		glide->setSuffix(QString::fromUtf8(" ms"));
		glide->setSpecialValueText(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Glide.Default")));

		auto *form = new QFormLayout();

		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.Zoom")), zoom);
		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.CentreX")), x);
		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.CentreY")), y);
		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Glide")), glide);

		auto *applyButton =
			new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Presets.Apply")), this);
		auto *renameButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Rename")), this);
		auto *removeButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Remove")), this);
		auto *upButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Presets.Up")), this);
		auto *downButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Presets.Down")), this);
		auto *closeButton =
			new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Presets.Close")), this);

		auto *buttons = new QHBoxLayout();

		buttons->addWidget(applyButton);
		buttons->addWidget(renameButton);
		buttons->addWidget(removeButton);
		buttons->addWidget(upButton);
		buttons->addWidget(downButton);
		buttons->addStretch(1);
		buttons->addWidget(closeButton);

		auto *intro = new QLabel(QString::fromUtf8(obs_module_text("Warp.Zoom.Presets.Intro")), this);

		intro->setWordWrap(true);

		layout->addWidget(intro);
		layout->addWidget(sources);
		layout->addWidget(presets, 1);
		layout->addLayout(form);
		layout->addLayout(buttons);

		connect(sources, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
		connect(presets, &QListWidget::itemSelectionChanged, this, [this]() { showSelected(); });
		connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

		connect(applyButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = currentSource();
			QString id = selectedId();
			warp_zoom_view view;

			if (!source || id.isEmpty())
				return;

			view.zoom = (float)(zoom->value() / 100.0);
			view.x = (float)(x->value() / 100.0);
			view.y = (float)(y->value() / 100.0);

			warp_zoom_source_update_preset(source, WARP_UTF8(id), nullptr, &view, glide->value());
			refresh();
		});

		connect(renameButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = currentSource();
			QString id = selectedId();
			bool ok = false;

			if (!source || id.isEmpty())
				return;

			QString name = QInputDialog::getText(
				this, QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Rename")),
				QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Save.Prompt")), QLineEdit::Normal,
				selectedName(), &ok);

			if (ok && !name.trimmed().isEmpty())
				warp_zoom_source_update_preset(source, WARP_UTF8(id), WARP_UTF8(name.trimmed()),
							       nullptr, -1);

			refresh();
		});

		connect(removeButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = currentSource();
			QString id = selectedId();

			if (source && !id.isEmpty())
				warp_zoom_source_remove_preset(source, WARP_UTF8(id));

			refresh();
		});

		connect(upButton, &QPushButton::clicked, this, [this]() { move(-1); });
		connect(downButton, &QPushButton::clicked, this, [this]() { move(1); });

		refresh();
	}

private:
	obs_source_t *currentSource() const
	{
		int index = sources->currentIndex();

		if (index < 0 || index >= chosen.size())
			return nullptr;

		return warpZoomTargetSource(chosen[index]);
	}

	QString selectedId() const
	{
		QListWidgetItem *item = presets->currentItem();

		return item ? item->data(Qt::UserRole).toString() : QString();
	}

	QString selectedName() const
	{
		QListWidgetItem *item = presets->currentItem();

		return item ? item->data(Qt::UserRole + 1).toString() : QString();
	}

	void move(int delta)
	{
		OBSSourceAutoRelease source = currentSource();
		QString id = selectedId();

		if (source && !id.isEmpty())
			warp_zoom_source_move_preset(source, WARP_UTF8(id), delta);

		refresh();
	}

	void refresh()
	{
		OBSSourceAutoRelease source = currentSource();
		QString selected = selectedId();

		rows = warpZoomReadPresets(source);
		presets->clear();

		for (int i = 0; i < rows.size(); i++) {
			auto *item = new QListWidgetItem(warpZoomPresetLabel(rows[i], i), presets);

			item->setData(Qt::UserRole, rows[i].id);
			item->setData(Qt::UserRole + 1, rows[i].name);
			item->setData(Qt::UserRole + 2, rows[i].fixed);

			if (rows[i].id == selected)
				presets->setCurrentItem(item);
		}

		showSelected();
	}

	void showSelected()
	{
		QListWidgetItem *item = presets->currentItem();
		bool editable = item && !item->data(Qt::UserRole + 2).toBool();
		int index = item ? presets->row(item) : -1;

		if (index >= 0 && index < rows.size()) {
			zoom->setValue(rows[index].view.zoom * 100.0);
			x->setValue(rows[index].view.x * 100.0);
			y->setValue(rows[index].view.y * 100.0);
			glide->setValue(rows[index].glide);
		}

		zoom->setEnabled(editable);
		x->setEnabled(editable);
		y->setEnabled(editable);
		glide->setEnabled(editable);
	}

	QComboBox *sources = nullptr;
	QListWidget *presets = nullptr;
	QDoubleSpinBox *zoom = nullptr;
	QDoubleSpinBox *x = nullptr;
	QDoubleSpinBox *y = nullptr;
	QSpinBox *glide = nullptr;

	QVector<WarpZoomTarget> chosen;
	QVector<WarpZoomPresetRow> rows;
};

} // namespace

extern "C" void warp_register_zoom_dock(void)
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	if (!main_window)
		return;

	auto *dock = new WarpZoomDock(main_window);

	dock->setObjectName("WarpZoomDock");

	if (!obs_frontend_add_dock_by_id("warp_zoom_dock", obs_module_text("Warp.Zoom.Dock.Title"), dock))
		delete dock;
}

void warp_open_zoom_presets(QWidget *parent)
{
	auto *dialog = new WarpZoomPresetsDialog(parent);

	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}
