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

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QSvgRenderer>
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

/* How big the pad and the zoom beside it are drawn.
 *
 * The pad is what a hand goes to mid-show, so it takes whatever room the dock
 * has rather than sitting at one size: the buttons are square and grow with
 * the dock's width up to something worth aiming at without looking, and give
 * that back down to a still-usable minimum when the dock is squeezed into a
 * strip beside the picture.
 *
 * What keeps the pad from crowding out the picture is the share of the dock's
 * height it is allowed - see `fitPad' - rather than these, which are only a
 * ceiling on how big a button is worth drawing at all: past a point an arrow is
 * aimed at without looking either way, and the room is better left to the
 * picture and the presets. Up to there the pad takes the dock's width, so the
 * buttons are as big as the row they sit in and stand a hair apart rather than
 * leaving a margin down either side. */
constexpr int WARP_ZOOM_PAD_MIN = 18;
constexpr int WARP_ZOOM_PAD_MAX = 96;
constexpr int WARP_ZOOM_PAD_MAX_MINIMAL = 128;

/* between the buttons, and the least that stands between the pad and the zoom
 * column, so the zoom reads as a control of its own rather than as a fourth
 * column of the pad */
constexpr int WARP_ZOOM_PAD_SPACING = 3;
constexpr int WARP_ZOOM_PAD_GAP = 6;

/* What a button comes down to rather than being laid out past the bottom of the
 * pad. A pad handed less height than the width it was given asks for draws
 * smaller buttons instead of hanging its top and bottom rows outside the panel,
 * and this is the size below which it stops giving that back. */
constexpr int WARP_ZOOM_PAD_FLOOR = 8;

/* How much of a button its drawing takes across the middle, the rest being the
 * room a button's face wants around anything on it. Half is what the arrows
 * came to while they were letters, so a pad looks the way it did. */
constexpr qreal WARP_ZOOM_GLYPH_SHARE = 0.5;

/* What the bars and their readouts hold open. A slider asks for a length worth
 * dragging and a readout for room to say "1000%", and between them they set
 * the floor for a dock that is meant to narrow to a strip; these are as small
 * as either goes while still being worth having. */
constexpr int WARP_ZOOM_SLIDER_MIN = 48;
constexpr int WARP_ZOOM_READOUT_MIN = 36;

/* The height the picture would like, and the least it is worth drawing in.
 *
 * The dock is a column with the picture at the top of it, and a dock dragged
 * short has to take that height off something.
 *
 * A dock panel is a child of the OBS window rather than a window of its own,
 * so nothing stops it being handed less height than the column inside it says
 * it needs, and a squeezed box layout hands its widgets slots smaller than
 * their minimums. A widget that has been told outright how short it may be
 * refuses that slot and is laid out at its own minimum from the top of it
 * instead - down over whatever comes after it, and behind them, since they
 * were built later and so are stacked above it. A picture with a floor of 180
 * is that widget: it is why the picture ends up behind the zoom bar and the
 * pad in a short dock.
 *
 * So the height it would rather have is a hint, which a layout is free to take
 * back, and the only height it holds is the floor here - the least it is worth
 * drawing a framing picture in at all. Below that there is no room for it and
 * it is put away, so the floor is never a slot it has to overflow, and the
 * room goes to the controls that are still worth working. */
constexpr int WARP_ZOOM_PREVIEW_WANT = 180;
constexpr int WARP_ZOOM_PREVIEW_FLOOR = 64;

/* How far under the dock the presets are shaded.
 *
 * A step of its own, so the slab is seen on the dark themes most of OBS is
 * worked in, where a share of a lightness that is already low comes to a
 * couple of values and to nothing anyone would notice; and a share of the
 * theme's lightness where that is the bigger of the two, so a light theme is
 * not given the same small step against a much brighter ground.
 *
 * A theme already as good as black has nothing left below it to take away, so
 * under the floor here the slab is lifted by the step instead - that it stands
 * out is the point, and which way round it stands out is the theme's business
 * rather than ours. */
constexpr int WARP_ZOOM_SECTION_STEP = 14;
constexpr int WARP_ZOOM_SECTION_SHARE = 8;
constexpr int WARP_ZOOM_SECTION_FLOOR = 24;

/* Puts back the whole range a widget's size can take.
 *
 * A theme sizes a button for the words on it, and the height it holds one to is
 * a height it stays at however big a square the pad hands out: geometry given
 * to a widget is cut to fit what its size is capped at, so the buttons come out
 * as wide as the pad has grown and as tall as a button with a word in it,
 * standing apart from the rows above and below by the difference.
 *
 * The cap is the theme's to set and it sets it whenever it dresses the widget,
 * which is after the button is built rather than during: answering it once at
 * build time is answering it too early. So this is asked again from the pad
 * each time it lays the buttons out, which is after anything that would have
 * put a cap back. */
void warpZoomFreeSize(QWidget *widget)
{
	/* asked only when it is not already so: a size constraint set again is
	 * a layout asked for again */
	if (widget->minimumWidth() || widget->minimumHeight())
		widget->setMinimumSize(0, 0);

	if (widget->maximumWidth() != QWIDGETSIZE_MAX || widget->maximumHeight() != QWIDGETSIZE_MAX)
		widget->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
}

/* Where the pad's drawings are, which is inside the plugin rather than beside
 * it: an arrow that has to be found on disk is an arrow that goes missing when
 * a plugin is copied about with less care than it was installed with. */
QString warpZoomDrawing(const char *name)
{
	return QString::fromUtf8(":/warp-zoom/icons/%1.svg").arg(QString::fromUtf8(name));
}

/* what a button with a drawing on it and no words says it is, to a tooltip and
 * to a screen reader alike */
void warpZoomName(QPushButton *button, const char *key)
{
	QString said = QString::fromUtf8(obs_module_text(key));

	button->setToolTip(said);
	button->setAccessibleName(said);
}

void warpZoomCompact(QPushButton *button)
{
	/* the pad hands out the geometry itself, so the button asks for
	 * nothing of its own and takes what it is given */
	button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
	/* a pad worked during a show should not take the keyboard away from
	 * whatever the operator is typing into */
	button->setFocusPolicy(Qt::NoFocus);

	/* These carry an arrow rather than words, so they take off the padding
	 * the theme leaves around one and say the same thing to it about their
	 * size as `warpZoomFreeSize' does, leaving the pad to be what says how
	 * big they are. */
	button->setStyleSheet(QString::fromUtf8("padding: 0px;"
						" min-width: 0px; min-height: 0px;"
						" max-width: %1px; max-height: %1px;")
				      .arg(QWIDGETSIZE_MAX));

	warpZoomFreeSize(button);
}

/* ------------------------------------------------------------------------- */
/* rows that wrap
 *
 * A row of ordinary buttons is what puts a floor under how narrow a panel can
 * be: five preset buttons side by side ask for more width than the pad and the
 * picture put together. Laid out like this they take the next line down
 * instead of holding the dock open, so the dock narrows to the width of one
 * button and the labels stay words rather than becoming guesswork. */
class WarpZoomFlow : public QLayout {
public:
	explicit WarpZoomFlow(int gap = 4)
	{
		setContentsMargins(0, 0, 0, 0);
		setSpacing(gap);
	}

	~WarpZoomFlow() override
	{
		while (QLayoutItem *item = takeAt(0))
			delete item;
	}

	void addItem(QLayoutItem *item) override { items.append(item); }

	int count() const override { return (int)items.size(); }

	QLayoutItem *itemAt(int index) const override { return items.value(index); }

	QLayoutItem *takeAt(int index) override
	{
		if (index < 0 || index >= items.size())
			return nullptr;

		return items.takeAt(index);
	}

	Qt::Orientations expandingDirections() const override { return {}; }

	bool hasHeightForWidth() const override { return true; }

	int heightForWidth(int width) const override { return run(QRect(0, 0, width, 0), false); }

	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		run(rect, true);
	}

	QSize sizeHint() const override { return minimumSize(); }

	/* one button wide is as narrow as a row of them goes */
	QSize minimumSize() const override
	{
		QMargins margins = contentsMargins();
		QSize size;

		for (QLayoutItem *item : items) {
			if (item->isEmpty())
				continue;

			size = size.expandedTo(item->minimumSize());
		}

		return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
	}

private:
	/* Walks the row, wrapping whatever does not fit onto the next line, and
	 * answers how tall that came to. The take and cancel buttons are only
	 * there while a shot is waiting, so anything hidden is stepped over
	 * rather than left holding a space. */
	int run(const QRect &outer, bool place) const
	{
		QMargins margins = contentsMargins();
		QRect rect = outer.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
		int x = rect.x();
		int y = rect.y();
		int line = 0;

		for (QLayoutItem *item : items) {
			if (item->isEmpty())
				continue;

			/* a control wider than the dock is given the dock's
			 * width and shortens what it says to suit */
			QSize wanted = item->sizeHint().boundedTo(QSize(rect.width(), QWIDGETSIZE_MAX));

			if (line && x + wanted.width() > rect.right() + 1) {
				x = rect.x();
				y += line + spacing();
				line = 0;
			}

			if (place)
				item->setGeometry(QRect(QPoint(x, y), wanted));

			x += wanted.width() + spacing();
			line = qMax(line, wanted.height());
		}

		return y + line - outer.y() + margins.bottom();
	}

	QList<QLayoutItem *> items;
};

/* A checkbox that gives up its words rather than the dock's width: the label
 * is shortened to whatever room there is, with the whole of it in the tooltip.
 * A sentence on a control is otherwise the widest thing in the panel and
 * decides how narrow the panel can be. */
class WarpZoomCheck : public QCheckBox {
public:
	explicit WarpZoomCheck(const QString &text, QWidget *parent = nullptr) : QCheckBox(text, parent), full(text)
	{
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		setToolTip(text);
	}

	/* The room the whole label would want, asked for however short the one
	 * on screen has been cut. Answered from the label as it stands, the
	 * control would ask for less every time it was laid out - each answer
	 * shorter than the last - and the words would walk off it a piece at a
	 * time until there were none left. */
	QSize sizeHint() const override { return QSize(beside() + wordsWidth(full), QCheckBox::sizeHint().height()); }

	/* the box and a word's worth beside it, which is as little as a control
	 * that can still be read and aimed at comes down to */
	QSize minimumSizeHint() const override
	{
		QSize hint = sizeHint();

		return QSize(qMin(hint.width(), beside() + fontMetrics().averageCharWidth() * 4), hint.height());
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QCheckBox::resizeEvent(event);
		elide();
	}

	/* What the control takes up beside its words is the style's to say, and
	 * a theme or a font picked mid-show is it saying something else. The
	 * whole label goes back on to be measured again, since what is on screen
	 * has been cut to a room that no longer applies. */
	void changeEvent(QEvent *event) override
	{
		QCheckBox::changeEvent(event);

		if (event->type() != QEvent::StyleChange && event->type() != QEvent::FontChange)
			return;

		room = -1;
		QCheckBox::setText(full);
		updateGeometry();
		elide();
	}

private:
	/* the words as the label lays them out, which is what has to be
	 * measured for the two answers below to agree */
	int wordsWidth(const QString &words) const { return fontMetrics().size(Qt::TextShowMnemonic, words).width(); }

	/* What the control takes up beside its words: the box, the space next to
	 * it, and whatever else the style puts around the whole thing.
	 *
	 * Taken off the control's own answer with the whole label on it rather
	 * than added up from the style's measurements by hand, because the two
	 * have to agree exactly. A hand-summed guess that comes to a few pixels
	 * more than the style leaves is a label cut short at the very width it
	 * asked for - the last word going to an ellipsis in a dock with room to
	 * spare - and one that comes to a few less is a label cut off by the
	 * edge of the control instead.
	 *
	 * Measured the once, while the label is still whole: asked again from a
	 * label already cut, the answer would be a style's minimum width shared
	 * out differently and the control would not settle. */
	int beside() const
	{
		if (room < 0 && text() == full)
			room = QCheckBox::sizeHint().width() - wordsWidth(full);

		return qMax(0, room);
	}

	void elide()
	{
		int words = qMax(0, width() - beside());
		QString shown = full;

		/* cut only once it will not fit as it stands, so that the last
		 * of the label is not lost to a rounding between how the words
		 * are measured and how the cut is made */
		if (wordsWidth(full) > words)
			shown = fontMetrics().elidedText(full, Qt::ElideRight, words);

		/* set only when it has actually changed: a label put back the
		 * same asks for the layout again and would not settle */
		if (shown != text())
			QCheckBox::setText(shown);
	}

	QString full;
	/* what the style leaves for the words, once it has been asked for */
	mutable int room = -1;
};

/* ------------------------------------------------------------------------- */
/* a section of the dock
 *
 * The presets are a thing of their own inside the dock - a list, the buttons
 * that work it, and the route a recall takes - and standing them on a slab a
 * shade under the panel behind them says so, rather than leaving them as more
 * rows in a column of rows. */

/* The slab's colour, taken from whatever the theme paints its panels rather
 * than written down here, so it follows OBS from a dark theme to a light one
 * and back without the dock having to know which one is on. */
QColor warpZoomSectionShade(const QColor &panel)
{
	int hue = 0;
	int saturation = 0;
	int lightness = 0;
	int alpha = 0;

	panel.getHsl(&hue, &saturation, &lightness, &alpha);

	int step = qMax(WARP_ZOOM_SECTION_STEP, lightness / WARP_ZOOM_SECTION_SHARE);
	int wanted = lightness > WARP_ZOOM_SECTION_FLOOR ? lightness - step : lightness + WARP_ZOOM_SECTION_STEP;

	return QColor::fromHsl(hue, saturation, qBound(0, wanted, 255), alpha);
}

class WarpZoomSection : public QFrame {
public:
	explicit WarpZoomSection(QWidget *parent = nullptr) : QFrame(parent)
	{
		setObjectName(QString::fromUtf8("warpZoomSection"));
		setFrameShape(QFrame::NoFrame);
		/* a plain container paints nothing of its own, and this is what
		 * has it draw the background the stylesheet below gives it */
		setAttribute(Qt::WA_StyledBackground, true);
		dress();
	}

protected:
	/* an operator picking another OBS theme is the palette changing under
	 * the dock, and the shade on it was mixed from the old one */
	void changeEvent(QEvent *event) override
	{
		QFrame::changeEvent(event);

		if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
			dressLater();
	}

private:
	/* The ground the slab stands on, which is the panel behind it rather
	 * than the slab itself.
	 *
	 * A stylesheet with a colour in it is written back into the widget's
	 * own palette when the style dresses it, so the window colour here is
	 * the shade this class last set rather than the theme's panel. Mixing
	 * the next shade from that would be mixing a shade from a shade, and
	 * since each pass takes another step away from the one before it, the
	 * colour never lands on the one already set and the dressing never
	 * settles. The panel behind us is the theme's own and nothing here
	 * writes to it, so it is what the shade is taken from. */
	QColor ground() const
	{
		const QWidget *behind = parentWidget();

		return behind ? behind->palette().color(QPalette::Window)
			      : QApplication::palette().color(QPalette::Window);
	}

	/* Asked for after the pass that brought us here has finished.
	 *
	 * A theme change is a style change to every widget and a palette change
	 * to every widget under the one that owns the palette, in whatever order
	 * Qt walks them, so the panel behind us may not have been given the new
	 * theme's colour at the moment we are told about it. Waiting for the
	 * event loop asks the panel once it has been dressed itself, and asking
	 * once for however many of those events arrive keeps a theme change to a
	 * single mix. */
	void dressLater()
	{
		if (dressPending)
			return;

		dressPending = true;
		QMetaObject::invokeMethod(
			this,
			[this]() {
				dressPending = false;
				dress();
			},
			Qt::QueuedConnection);
	}

	/* Dressed only when the colour has actually moved. Setting a stylesheet
	 * is itself a style change, which is one of the things that brings us
	 * here, so dressing on every pass would never settle. */
	void dress()
	{
		QColor wanted = warpZoomSectionShade(ground());

		if (wanted == shade)
			return;

		shade = wanted;
		setStyleSheet(QString::fromUtf8("QFrame#warpZoomSection { background-color: %1;"
						" border-radius: 4px; }")
				      .arg(shade.name(QColor::HexRgb)));
	}

	QColor shade;
	bool dressPending = false;
};

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

	/* Which thing this is, which is not the same question as what it is
	 * called: a source renamed mid-show is the source the dock was already
	 * on, and the dock stays on it rather than counting it as gone and
	 * moving off to whatever is first in the list. */
	bool operator==(const WarpZoomTarget &other) const
	{
		return parentUuid == other.parentUuid && filterName == other.filterName;
	}

	bool operator!=(const WarpZoomTarget &other) const { return !(*this == other); }
};

/* Whether the list on screen is still the list there is - the same things, in
 * the same order, under the same names.
 *
 * The names are asked about here and nowhere else. Being the same thing is what
 * `operator==' answers, and a rename does not change that, so a list compared
 * on that alone reads as unchanged and the dock goes on offering the name the
 * source used to have. */
bool warpZoomSameList(const QVector<WarpZoomTarget> &shown, const QVector<WarpZoomTarget> &found)
{
	if (shown.size() != found.size())
		return false;

	for (int index = 0; index < shown.size(); index++) {
		if (shown[index] != found[index] || shown[index].label != found[index].label)
			return false;
	}

	return true;
}

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
		/* The height it would rather have is asked for in the hint and
		 * the floor is all it holds, so a dock dragged short takes the
		 * room out of the picture - which has the most to give and the
		 * least to lose by giving it - before anything is squeezed.
		 *
		 * Nothing is asked for across: a picture is drawn to whatever
		 * width there is, and a dock meant to narrow to a strip beside
		 * the canvas cannot be held open by one. */
		setMinimumHeight(WARP_ZOOM_PREVIEW_FLOOR);
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
		setCursor(Qt::OpenHandCursor);
		setMouseTracking(false);
	}

	/* sixteen by nine of the height it wants, so the room it asks for is the
	 * shape of what goes in it */
	QSize sizeHint() const override { return QSize(WARP_ZOOM_PREVIEW_WANT * 16 / 9, WARP_ZOOM_PREVIEW_WANT); }

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
	int path;
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
		row.path = (int)obs_data_get_int(item, WARP_ZOOM_P_PATH);
		row.fixed = obs_data_get_bool(item, WARP_ZOOM_P_FIXED);

		rows.append(row);
		obs_data_release(item);
	}

	obs_data_array_release(array);

	return rows;
}

/* How a preset reads in a list: the reset position stands on its name, and the
 * ones after it carry the number of the slot hotkey that fires them. */
QString warpZoomPresetLabel(const WarpZoomPresetRow &row, int index)
{
	if (row.fixed)
		return row.name;

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

/* The reset position, drawn as what it is on a PTZ desk: the dot in the middle
 * of the pad. A word does not fit in a button this small, and the dot is read
 * at a glance by anyone who has worked a camera desk. */
/* A pad button, which is a drawing on a button rather than a letter on one.
 *
 * The arrows were characters typed into the buttons - the same ones a document
 * would use for them - and a character is only ever as good as the font it is
 * asked of: the corners in particular are missing from plenty of them, and what
 * stands in for a missing one is a box or somebody else's idea of the shape.
 * They were sized by setting the font, so how much of the button an arrow took
 * was the typeface's business too, and the four corners never quite agreed with
 * the four straights.
 *
 * These are drawn from vectors of the plugin's own instead, compiled in rather
 * than read off disk, and drawn at whatever size the button has been given -
 * so they are as sharp on a large pad as a small one, and the same on every
 * machine. The drawing says only the shape; the colour is the theme's, taken
 * from the same place the words on any other button take theirs. */
class WarpZoomPadButton : public QPushButton {
public:
	WarpZoomPadButton(const QString &drawing, QWidget *parent = nullptr) : QPushButton(parent), glyph(drawing) {}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		QPushButton::paintEvent(event);

		int room = side();
		QPixmap mark = drawn(room);
		QPainter painter(this);

		painter.drawPixmap(QPointF((width() - room) / 2.0, (height() - room) / 2.0), mark);
	}

private:
	/* how much of the button the drawing takes, the rest being the room a
	 * button's face needs around anything on it */
	int side() const { return qMax(1, qRound(qMin(width(), height()) * WARP_ZOOM_GLYPH_SHARE)); }

	/* Kept until something about it changes rather than drawn again every
	 * time the button is painted, since a pad is repainted for every hover
	 * and every press. The theme's colour is part of what is asked for, so
	 * a theme picked mid-show redraws it by asking for something else. */
	QPixmap drawn(int room)
	{
		qreal ratio = devicePixelRatioF();
		QColor ink = palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText);

		if (!sheet.isNull() && room == sheetSide && ink == sheetInk && qFuzzyCompare(ratio, sheetRatio))
			return sheet;

		QPainter painter;

		/* asked for in the screen's own pixels and told what they are
		 * worth, so the drawing is sharp on a display that has more of
		 * them than it has points */
		sheet = QPixmap(QSize(room, room) * ratio);
		sheet.setDevicePixelRatio(ratio);
		sheet.fill(Qt::transparent);

		painter.begin(&sheet);
		painter.setRenderHint(QPainter::Antialiasing, true);
		glyph.render(&painter, QRectF(0, 0, room, room));

		/* the shape is kept and the colour thrown away: what is left
		 * where the drawing put ink is the theme's colour instead */
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(QRectF(0, 0, room, room), ink);
		painter.end();

		sheetSide = room;
		sheetInk = ink;
		sheetRatio = ratio;

		return sheet;
	}

	QSvgRenderer glyph;
	QPixmap sheet;
	QColor sheetInk;
	int sheetSide = 0;
	qreal sheetRatio = 0.0;
};

/* The pad itself: eight directions around the reset dot, with the zoom
 * standing beside it as its own column.
 *
 * The buttons are placed by hand rather than by a grid, because what matters
 * is that they stay square and as big as the dock's width allows: a grid would
 * hand out whatever space there was and leave the arrows in letterboxes. The
 * pad works out the biggest square that four columns of them fit into, lays
 * itself out at that size, and sits in the middle of whatever it was given. */
class WarpZoomPad : public QWidget {
public:
	explicit WarpZoomPad(QWidget *parent = nullptr) : QWidget(parent)
	{
		QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);

		/* the buttons are square, so the row the pad sits in takes only
		 * the height the width it was given works out to */
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
	}

	/* Columns 0 to 2 are the pad; column 3 is the zoom beside it. */
	void place(QWidget *widget, int row, int column)
	{
		Cell cell;

		widget->setParent(this);

		cell.widget = widget;
		cell.row = row;
		cell.column = column;

		cells.append(cell);
	}

	/* How big a button is allowed to get, which is the whole difference
	 * between the pad beside a picture and the pad on its own. */
	void setLargest(int size)
	{
		if (largest == size)
			return;

		largest = size;
		updateGeometry();
		arrange();
	}

	bool hasHeightForWidth() const override { return true; }

	int heightForWidth(int width) const override { return down(sizeFor(width)); }

	QSize minimumSizeHint() const override { return QSize(across(WARP_ZOOM_PAD_MIN), down(WARP_ZOOM_PAD_MIN)); }

	QSize sizeHint() const override { return QSize(across(largest), down(largest)); }

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		arrange();
	}

	/* A theme is dressed onto the buttons after they are built, and again
	 * whenever the operator picks another one, and what it has to say about
	 * how big a button may be would hold from then on. The pad lays itself
	 * out once more here so that what it hands out is what is on screen. */
	void changeEvent(QEvent *event) override
	{
		QWidget::changeEvent(event);

		if (event->type() == QEvent::StyleChange)
			arrange();
	}

private:
	struct Cell {
		QWidget *widget;
		int row;
		int column;
	};

	/* the gap grows with the buttons, so the zoom keeps standing apart
	 * rather than crowding the pad as both get bigger */
	static int gapFor(int size) { return qMax(WARP_ZOOM_PAD_GAP, size / 3); }

	static int down(int size) { return size * 3 + WARP_ZOOM_PAD_SPACING * 2; }

	static int across(int size) { return size * 4 + WARP_ZOOM_PAD_SPACING * 2 + gapFor(size); }

	/* The biggest square three rows of them fit down into - `down' asked the
	 * other way round.
	 *
	 * The pad asks for the height the width it was given works out to, and
	 * in a dock with a picture, a preset list and the rows under them all
	 * wanting height at once it can be handed less than it asked for. What
	 * it is given is the answer, not what it asked for. */
	static int downTo(int height) { return (height - WARP_ZOOM_PAD_SPACING * 2) / 3; }

	/* The biggest square four columns of them fit into: worked out once
	 * against the smallest gap there can be, and again against the gap that
	 * size asks for. */
	int sizeFor(int width) const
	{
		int size =
			qBound(WARP_ZOOM_PAD_MIN, (width - WARP_ZOOM_PAD_SPACING * 2 - WARP_ZOOM_PAD_GAP) / 4, largest);

		return qBound(WARP_ZOOM_PAD_MIN, (width - WARP_ZOOM_PAD_SPACING * 2 - gapFor(size)) / 4, largest);
	}

	void arrange()
	{
		/* square: the size the width allows, or the height, whichever
		 * there is less room for */
		int size = qMax(WARP_ZOOM_PAD_FLOOR, qMin(sizeFor(width()), downTo(height())));
		int gap = gapFor(size);
		int left = (width() - across(size)) / 2;
		int top = (height() - down(size)) / 2;

		for (const Cell &cell : cells) {
			int x = cell.column < 3 ? left + cell.column * (size + WARP_ZOOM_PAD_SPACING)
						: left + size * 3 + WARP_ZOOM_PAD_SPACING * 2 + gap;

			/* the square is only handed out if the widget will take
			 * one: whatever the theme has capped it at is put back
			 * first */
			warpZoomFreeSize(cell.widget);

			cell.widget->setGeometry(x, top + cell.row * (size + WARP_ZOOM_PAD_SPACING), size, size);
		}
	}

	QVector<Cell> cells;
	int largest = WARP_ZOOM_PAD_MAX;
};

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

	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		fitParts();
	}

	/* A theme or a font picked mid-show changes what every row in the dock
	 * takes up, and with it how much height there is left for the picture. */
	void changeEvent(QEvent *event) override
	{
		QWidget::changeEvent(event);

		if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)
			fitParts();
	}

private:
	/* What the dock's height is shared out to, worked out from the top down:
	 * whether there is room for the picture at all, and then how big the pad
	 * may grow in what is left. */
	void fitParts()
	{
		fitPreview();
		fitPad();
	}

	/* The height the rest of the dock holds, which is what says whether the
	 * picture has anywhere to go.
	 *
	 * Asked of the rows themselves rather than counted up here, so a theme
	 * with taller buttons or an operator on a bigger font is answered as it
	 * stands rather than against a number written down when this was
	 * written. The picture is left out of it - it is the one being placed -
	 * and so is anything hidden, since a row that is not there holds
	 * nothing. */
	int restHeight() const
	{
		QLayout *box = layout();
		QMargins margins = box->contentsMargins();
		int total = margins.top() + margins.bottom();
		int rows = 0;

		for (int i = 0; i < box->count(); i++) {
			QLayoutItem *item = box->itemAt(i);

			if (item->widget() == preview || item->isEmpty())
				continue;

			total += item->minimumSize().height();
			rows++;
		}

		/* the gaps between them, and the one the picture would stand
		 * above if it were put back */
		return total + box->spacing() * rows;
	}

	/* Whether the picture is worth the room it would take. Below the floor
	 * it is a strip too thin to frame a shot in, and the pad, the presets
	 * and the speed are what an operator can still work in a dock that
	 * short, so it is put away rather than laid out under them. */
	void fitPreview()
	{
		bool room = !minimal && height() - restHeight() >= WARP_ZOOM_PREVIEW_FLOOR;

		/* asked of what the dock was told rather than of what is on
		 * screen: a child of a panel that has not been shown yet is not
		 * visible whatever the dock has decided about it */
		if (room == preview->isHidden())
			preview->setVisible(room);
	}

	/* How big the pad is let grow, which is a question about the dock's
	 * height rather than its width.
	 *
	 * The pad takes its size from the width it is given, and a dock kept
	 * short and wide - one along the bottom of the window - would hand it
	 * a pad taller than the panel it is in, with the picture and the
	 * presets squeezed out behind it. So the width it works from is
	 * capped by the share of the height the pad can have: a third of the
	 * dock beside a picture, half of it with the picture put away. */
	void fitPad()
	{
		int share = minimal ? height() / 2 : height() / 3;
		int room = (share - WARP_ZOOM_PAD_SPACING * 2) / 3;

		pad->setLargest(
			qBound(WARP_ZOOM_PAD_MIN, room, minimal ? WARP_ZOOM_PAD_MAX_MINIMAL : WARP_ZOOM_PAD_MAX));
	}

	/* Minimal drops the picture and nothing else: the pad, the presets and
	 * the speed are what an operator works, and without the picture there
	 * is no per-frame grab to pay for either.
	 *
	 * With the picture gone the pad is what the panel is for, so it is let
	 * grow past what it stops at while it is sharing the dock with one. */
	void setMinimal(bool value)
	{
		minimal = value;
		fitParts();
	}

	void build()
	{
		auto *layout = new QVBoxLayout(this);

		/* the dock is meant to go down to a strip beside the picture,
		 * and margins are width like anything else */
		layout->setContentsMargins(4, 4, 4, 4);

		/* which source is being framed; the name of a source is longer
		 * than a narrow dock, so the list is let shrink past it and
		 * says the whole of it in the tooltip instead */
		targets = new QComboBox(this);
		targets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		targets->setMinimumContentsLength(8);
		targets->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

		/* what the dropdown is set from when it is left to itself, so it
		 * reads as the setting on the control above it rather than as
		 * something competing with it for the same row */
		follow = new WarpZoomCheck(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Follow")), this);
		follow->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Follow.Desc")));

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
		/* a bar has a length it would rather be; in a dock narrowed to a
		 * strip it takes what is left over instead */
		zoomSlider->setMinimumWidth(WARP_ZOOM_SLIDER_MIN);

		zoomLabel = new QLabel(this);
		zoomLabel->setMinimumWidth(WARP_ZOOM_READOUT_MIN);
		zoomLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		auto *zoomRow = new QHBoxLayout();

		zoomRow->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Warp.Zoom.Zoom")), this));
		zoomRow->addWidget(zoomSlider, 1);
		zoomRow->addWidget(zoomLabel);

		/* The pad: eight directions around the reset dot, laid out the
		 * way a PTZ desk is, with the zoom standing beside it as its
		 * own column - in on top, out below - the way the rocker on a
		 * camera desk sits to the right of the stick. A corner moves a
		 * full step on both axes, the way a joystick held into its
		 * corner covers more ground than one pushed straight. */
		pad = new WarpZoomPad(this);

		auto *up = new WarpZoomPadButton(warpZoomDrawing("pan-up"), pad);
		auto *down = new WarpZoomPadButton(warpZoomDrawing("pan-down"), pad);
		auto *left = new WarpZoomPadButton(warpZoomDrawing("pan-left"), pad);
		auto *right = new WarpZoomPadButton(warpZoomDrawing("pan-right"), pad);
		auto *upLeft = new WarpZoomPadButton(warpZoomDrawing("pan-up-left"), pad);
		auto *upRight = new WarpZoomPadButton(warpZoomDrawing("pan-up-right"), pad);
		auto *downLeft = new WarpZoomPadButton(warpZoomDrawing("pan-down-left"), pad);
		auto *downRight = new WarpZoomPadButton(warpZoomDrawing("pan-down-right"), pad);
		auto *in = new WarpZoomPadButton(warpZoomDrawing("zoom-in"), pad);
		auto *out = new WarpZoomPadButton(warpZoomDrawing("zoom-out"), pad);

		resetButton = new WarpZoomPadButton(warpZoomDrawing("reset"), pad);
		resetButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Reset.Desc")));
		resetButton->setAccessibleName(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Reset")));

		in->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.In")));
		out->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Out")));

		/* A drawing says which way a button goes to anyone looking at it
		 * and nothing at all to anyone who is not, which the arrows it
		 * replaced at least did by being letters. Every one of them says
		 * what it is in words, for the tooltip and for a reader. */
		warpZoomName(up, "Warp.Zoom.Dock.Pan.Up");
		warpZoomName(down, "Warp.Zoom.Dock.Pan.Down");
		warpZoomName(left, "Warp.Zoom.Dock.Pan.Left");
		warpZoomName(right, "Warp.Zoom.Dock.Pan.Right");
		warpZoomName(upLeft, "Warp.Zoom.Dock.Pan.UpLeft");
		warpZoomName(upRight, "Warp.Zoom.Dock.Pan.UpRight");
		warpZoomName(downLeft, "Warp.Zoom.Dock.Pan.DownLeft");
		warpZoomName(downRight, "Warp.Zoom.Dock.Pan.DownRight");
		in->setAccessibleName(in->toolTip());
		out->setAccessibleName(out->toolTip());

		pad->place(upLeft, 0, 0);
		pad->place(up, 0, 1);
		pad->place(upRight, 0, 2);
		pad->place(left, 1, 0);
		pad->place(resetButton, 1, 1);
		pad->place(right, 1, 2);
		pad->place(downLeft, 2, 0);
		pad->place(down, 2, 1);
		pad->place(downRight, 2, 2);
		pad->place(in, 0, 3);
		pad->place(out, 2, 3);

		for (WarpZoomPadButton *button :
		     {up, down, left, right, upLeft, upRight, downLeft, downRight, in, out, resetButton})
			warpZoomCompact(button);

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
		confirm = new WarpZoomCheck(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Confirm")), this);
		confirm->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Confirm.Desc")));

		takeButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Take")), this);
		dropButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Drop")), this);

		takeButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Take.Desc")));
		takeButton->setVisible(false);
		dropButton->setVisible(false);

		auto *confirmRow = new WarpZoomFlow();

		confirmRow->addWidget(confirm);
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
		/* a preset reads as much of its name as there is room for, which
		 * is not a reason to hold the dock open */
		presets->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
		presets->setTextElideMode(Qt::ElideRight);

		auto *saveButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Save")), this);
		auto *recallButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Recall")), this);

		updateButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Update")), this);
		renameButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Rename")), this);
		removeButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Remove")), this);

		saveButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Save.Desc")));
		updateButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Update.Desc")));

		auto *presetButtons = new WarpZoomFlow();

		presetButtons->addWidget(recallButton);
		presetButtons->addWidget(saveButton);
		presetButtons->addWidget(updateButton);
		presetButtons->addWidget(renameButton);
		presetButtons->addWidget(removeButton);

		/* The route the move to the picked preset takes, which is worth
		 * changing while watching it happen rather than in a window
		 * somewhere else: it is a thing you judge by eye. */
		path = new QComboBox(this);
		path->addItem(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Path.Direct")),
			      WARP_ZOOM_PATH_DIRECT);
		path->addItem(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Path.Arc")), WARP_ZOOM_PATH_ARC);
		path->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Path.Desc")));
		path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

		auto *pathRow = new QHBoxLayout();

		pathRow->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Path")), this));
		pathRow->addWidget(path, 1);

		connect(path, &QComboBox::currentIndexChanged, this, [this](int index) {
			OBSSourceAutoRelease source = zoomSource();
			QString id = selectedPreset();

			if (loading || !source || id.isEmpty() || index < 0)
				return;

			warp_zoom_source_update_preset(source, WARP_UTF8(id), nullptr, nullptr, -1,
						       path->itemData(index).toInt());
			refreshLists();
		});

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
				warp_zoom_source_update_preset(source, WARP_UTF8(id), nullptr, &view, -1, -1);

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
							       nullptr, -1, -1);

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
		speedSlider->setMinimumWidth(WARP_ZOOM_SLIDER_MIN);

		speedLabel = new QLabel(this);
		speedLabel->setMinimumWidth(WARP_ZOOM_READOUT_MIN);
		speedLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		auto *speedRow = new QHBoxLayout();

		speedRow->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Warp.Video.Speed")), this));
		speedRow->addWidget(speedSlider, 1);
		speedRow->addWidget(speedLabel);

		auto *speedButtons = new WarpZoomFlow();

		for (int speed : warpZoomSpeeds) {
			auto *button = new QPushButton(QString("%1%").arg(speed), this);

			connect(button, &QPushButton::clicked, this, [this, speed]() { setSpeed(speed); });
			speedButtons->addWidget(button);
		}

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

		/* The presets stand on a slab of their own: the list, the
		 * buttons that work it and the route a recall takes are one
		 * thing an operator sets a show up with, and the dock says so
		 * by shading the ground under them rather than by running them
		 * in with the rows above and below. */
		auto *presetSection = new WarpZoomSection(this);
		auto *presetBox = new QVBoxLayout(presetSection);

		presetBox->setContentsMargins(6, 6, 6, 6);
		presetBox->addWidget(
			new QLabel(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Presets")), presetSection));
		presetBox->addWidget(presets, 1);
		presetBox->addLayout(presetButtons);
		presetBox->addLayout(pathRow);

		layout->addWidget(targets);
		layout->addWidget(follow);
		layout->addWidget(preview, 1);
		layout->addLayout(zoomRow);
		layout->addWidget(pad);
		layout->addLayout(confirmRow);
		layout->addWidget(presetSection, 1);
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

		loading = true;

		path->setEnabled(editable);
		path->setCurrentIndex(item ? path->findData(item->data(Qt::UserRole + 3).toInt()) : 0);

		loading = false;
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

		if (!warpZoomSameList(chosen, found)) {
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

		/* the same thing, under whatever it is called now: what the dock
		 * holds on to carries the old name until it is taken from the
		 * list again */
		if (index >= 0)
			target = chosen[index];

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
		/* what the list says is cut to the dock's width, so the whole
		 * name is kept where it can still be read */
		targets->setToolTip(targets->currentText());

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
		/* What the list is compared against, which is what is shown plus
		 * what is carried without being shown: a preset whose route was
		 * changed reads the same and still has to be taken again. */
		QStringList shape;

		for (int i = 0; i < rows.size(); i++) {
			labels << warpZoomPresetLabel(rows[i], i);
			shape << QString("%1\n%2").arg(labels.last()).arg(rows[i].path);
		}

		/* the list is rebuilt only when it has actually changed, so a
		 * preset stays selected while the dock ticks over */
		if (shape == shownPresets)
			return;

		shownPresets = shape;
		presets->clear();

		for (int i = 0; i < rows.size(); i++) {
			auto *item = new QListWidgetItem(labels[i], presets);

			item->setData(Qt::UserRole, rows[i].id);
			item->setData(Qt::UserRole + 1, rows[i].name);
			item->setData(Qt::UserRole + 2, rows[i].fixed);
			item->setData(Qt::UserRole + 3, rows[i].path);

			if (rows[i].id == selected)
				presets->setCurrentItem(item);
		}

		updateButtons();
	}

	void refreshView()
	{
		OBSSourceAutoRelease source = zoomSource();
		warp_zoom_view view = warp_zoom_default_view();
		warp_zoom_view heading = warp_zoom_default_view();
		warp_zoom_view stage = warp_zoom_default_view();
		bool have = source && warp_zoom_source_get(source, &view, &heading);
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
		const warp_zoom_view &shown = is_staged ? stage : heading;

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

		/* With the picture away - put away for good in minimal, or for
		 * want of height in a dock dragged short - there is nothing to
		 * draw, so nothing is rendered and the dock costs only what its
		 * widgets cost. */
		if (preview->isHidden()) {
			refreshView();
			return;
		}

		OBSSourceAutoRelease parent = warpZoomParent(target);

		preview->setLive(parent != nullptr);
		preview->setImage(parent ? grabber.grab(parent) : QImage());

		refreshView();
	}

	QComboBox *targets = nullptr;
	QComboBox *path = nullptr;
	QCheckBox *follow = nullptr;
	QCheckBox *confirm = nullptr;
	QPushButton *takeButton = nullptr;
	QPushButton *dropButton = nullptr;
	WarpZoomPreview *preview = nullptr;
	WarpZoomPad *pad = nullptr;
	QSlider *zoomSlider = nullptr;
	QLabel *zoomLabel = nullptr;
	WarpZoomPadButton *resetButton = nullptr;
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

		path = new QComboBox(this);
		path->addItem(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Path.Direct")),
			      WARP_ZOOM_PATH_DIRECT);
		path->addItem(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Path.Arc")), WARP_ZOOM_PATH_ARC);
		path->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Path.Desc")));

		auto *form = new QFormLayout();

		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.Zoom")), zoom);
		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.CentreX")), x);
		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.CentreY")), y);
		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Glide")), glide);
		form->addRow(QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Path")), path);

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

			warp_zoom_source_update_preset(source, WARP_UTF8(id), nullptr, &view, glide->value(),
						       path->currentData().toInt());
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
							       nullptr, -1, -1);

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
			path->setCurrentIndex(path->findData(rows[index].path));
		}

		zoom->setEnabled(editable);
		x->setEnabled(editable);
		y->setEnabled(editable);
		glide->setEnabled(editable);
		path->setEnabled(editable);
	}

	QComboBox *sources = nullptr;
	QListWidget *presets = nullptr;
	QDoubleSpinBox *zoom = nullptr;
	QDoubleSpinBox *x = nullptr;
	QDoubleSpinBox *y = nullptr;
	QSpinBox *glide = nullptr;
	QComboBox *path = nullptr;

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
