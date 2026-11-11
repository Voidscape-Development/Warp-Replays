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
#include <util/platform.h>

#include <QApplication>
#include <QAbstractButton>
#include <QBoxLayout>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHelpEvent>
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
#include <QScrollArea>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolTip>
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

/* how old the frame left from the grab before can be and still be shown: a few
 * grabs at the resting rate, so a dock that has been put away and brought back
 * does not open on what was on air when it went */
constexpr uint64_t WARP_ZOOM_GRAB_STALE_NS = 250000000;

/* how far one press of the dock's pan buttons moves, as a fraction of what is
 * on screen, matching the pan hotkeys */
constexpr float WARP_ZOOM_DOCK_PAN = WARP_ZOOM_PAN_STEP;

/* the speeds the dock offers as buttons, which are the ones the speed hotkeys
 * are bound to */
const int warpZoomSpeeds[] = {25, 50, 100, 150, 200};

/* How big the pad is drawn.
 *
 * The pad is what a hand goes to mid-show, but it is not the only way to move
 * the picture - the picture itself is dragged - so it is kept to buttons worth
 * aiming at rather than given the dock: square, growing with the room there is
 * up to a size a thumb lands on without looking, and coming back down to a
 * still-usable minimum when the dock is squeezed into a strip.
 *
 * What it grows into is a share of the row it sits in and a share of the dock's
 * height - see `fitPad' - with the zoom standing beside it in what is left, so
 * the presets and the picture keep their room. With the picture put away the
 * pad is let grow a little further, since it is then the only way to pan. */
constexpr int WARP_ZOOM_PAD_MIN = 18;
constexpr int WARP_ZOOM_PAD_MAX = 48;
constexpr int WARP_ZOOM_PAD_MAX_MINIMAL = 64;

/* between the buttons */
constexpr int WARP_ZOOM_PAD_SPACING = 3;

/* how much of the row it shares with the zoom the pad may take, in hundredths */
constexpr int WARP_ZOOM_PAD_SHARE = 45;

/* What a button comes down to rather than being laid out past the bottom of the
 * pad. A pad handed less height than the width it was given asks for draws
 * smaller buttons instead of hanging its top and bottom rows outside the panel,
 * and this is the size below which it stops giving that back. */
constexpr int WARP_ZOOM_PAD_FLOOR = 8;

/* The zoom in and out buttons either side of the readout, which follow the size
 * of the pad's buttons between these so the two read as one set of controls. */
constexpr int WARP_ZOOM_ROCKER_MIN = 22;
constexpr int WARP_ZOOM_ROCKER_MAX = 44;

/* The width under which the pad and the zoom stop standing side by side and
 * stack instead, so a dock narrowed to a strip keeps both rather than squeezing
 * either out. */
constexpr int WARP_ZOOM_STACK_BELOW = 260;

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

/* How finely the zoom bar is cut.
 *
 * The bar runs along the logarithm of the zoom rather than the zoom itself:
 * every other zoom control multiplies, so the same length of bar should be the
 * same step on screen at 600% as at 150%. Run straight, the first doubling -
 * where most framing is done - would be an eighth of the bar and the last one
 * half of it. */
constexpr int WARP_ZOOM_BAR_STEPS = 1000;

/* the zooms the dock offers as buttons under the bar: each doubling there is */
const float warpZoomStops[] = {1.0f, 2.0f, 4.0f, 8.0f};

/* The narrowest a preset tile is worth drawing with its map and its name side
 * by side, which is what says how many of them stand in a row. */
constexpr int WARP_ZOOM_TILE_MIN = 132;

/* How wide a dock has to be, and how much wider than it is tall, before it is
 * laid out as columns side by side rather than as one column: the shape a dock
 * has along the bottom of the OBS window. */
constexpr int WARP_ZOOM_WIDE_MIN = 640;

/* the colour of a shot that is waiting, wherever one is drawn */
inline QColor warpZoomStageColor()
{
	return QColor(224, 152, 44);
}

/* how close two framings have to be to count as the same one: a preset that has
 * been recalled is where the picture is, give or take what a float loses */
bool warpZoomSameView(const warp_zoom_view &a, const warp_zoom_view &b)
{
	return std::fabs(a.zoom - b.zoom) < 0.005f && std::fabs(a.x - b.x) < 0.002f && std::fabs(a.y - b.y) < 0.002f;
}

int warpZoomToBar(float zoom)
{
	double span = std::log((double)WARP_ZOOM_MAX / WARP_ZOOM_MIN);
	double at = std::log((double)qBound(WARP_ZOOM_MIN, zoom, WARP_ZOOM_MAX) / WARP_ZOOM_MIN) / span;

	return qRound(at * WARP_ZOOM_BAR_STEPS);
}

float warpZoomFromBar(int value)
{
	double at = (double)qBound(0, value, WARP_ZOOM_BAR_STEPS) / WARP_ZOOM_BAR_STEPS;

	return (float)(WARP_ZOOM_MIN * std::pow((double)WARP_ZOOM_MAX / WARP_ZOOM_MIN, at));
}

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
	/* 'fill' hands whatever is left of each line to the last thing on it,
	 * for a row that ends in the control it is there for */
	explicit WarpZoomFlow(int gap = 4, bool fill = false) : filling(fill)
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
	struct Placed {
		QLayoutItem *item;
		QRect slot;
	};

	/* Walks the row, wrapping whatever does not fit onto the next line, and
	 * answers how tall that came to. Some of what is in a row is only there
	 * some of the time, so anything hidden is stepped over rather than left
	 * holding a space. */
	int run(const QRect &outer, bool place) const
	{
		QMargins margins = contentsMargins();
		QRect rect = outer.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
		QVector<Placed> row;
		int x = rect.x();
		int y = rect.y();
		int line = 0;

		/* A line is placed once it is known how tall it came to, so a
		 * label standing in a row of buttons is given the height of the
		 * row and sits in the middle of it. */
		auto flush = [&]() {
			for (int i = 0; place && i < row.size(); i++) {
				QRect slot = row[i].slot;

				slot.setHeight(line);

				if (filling && i == row.size() - 1)
					slot.setRight(qMax(slot.right(), rect.right()));

				row[i].item->setGeometry(slot);
			}

			row.clear();
		};

		for (QLayoutItem *item : items) {
			if (item->isEmpty())
				continue;

			/* a control wider than the dock is given the dock's
			 * width and shortens what it says to suit */
			QSize wanted = item->sizeHint().boundedTo(QSize(rect.width(), QWIDGETSIZE_MAX));

			if (line && x + wanted.width() > rect.right() + 1) {
				flush();
				x = rect.x();
				y += line + spacing();
				line = 0;
			}

			row.append({item, QRect(QPoint(x, y), wanted)});

			x += wanted.width() + spacing();
			line = qMax(line, wanted.height());
		}

		flush();

		return y + line - outer.y() + margins.bottom();
	}

	QList<QLayoutItem *> items;
	bool filling = false;
};

/* ------------------------------------------------------------------------- */
/* cells of one size
 *
 * The preset tiles, the preset speeds and the zoom stops are each a set of
 * things that are the same kind of thing, and they read as a set by being the
 * same size: as many to a line as fit at the width a cell is worth having, all
 * of them as wide as each other, and the next line down for the rest. A dock
 * with the room gets them in one row; a dock narrowed to a strip gets them one
 * under another, and nothing in between has to be chosen. */
class WarpZoomCells : public QLayout {
public:
	/* 'least' is the narrowest a cell is worth, or zero for whatever the
	 * widest thing in it asks for; 'most' is how many go on a line at the
	 * outside, or zero for as many as fit */
	explicit WarpZoomCells(int least = 0, int most = 0, int gap = 4) : cellLeast(least), cellMost(most)
	{
		setContentsMargins(0, 0, 0, 0);
		setSpacing(gap);
	}

	~WarpZoomCells() override
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

	Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

	bool hasHeightForWidth() const override { return true; }

	int heightForWidth(int width) const override { return run(QRect(0, 0, width, 0), false); }

	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		run(rect, true);
	}

	QSize sizeHint() const override
	{
		QSize cell = cellSize();
		int across = qMax(1, shown());

		if (cellMost > 0)
			across = qMin(across, cellMost);

		return QSize(cell.width() * across + spacing() * (across - 1), cell.height());
	}

	/* one cell, at the least it will go to, is as small as a set of them
	 * gets */
	QSize minimumSize() const override
	{
		QSize size;

		for (QLayoutItem *item : items) {
			if (!item->isEmpty())
				size = size.expandedTo(item->minimumSize());
		}

		return QSize(size.width(), cellSize().height());
	}

private:
	int shown() const
	{
		int total = 0;

		for (QLayoutItem *item : items) {
			if (!item->isEmpty())
				total++;
		}

		return total;
	}

	/* the width a cell is worth and the height every one of them is given */
	QSize cellSize() const
	{
		QSize size(cellLeast, 0);

		for (QLayoutItem *item : items) {
			if (item->isEmpty())
				continue;

			/* asked of the widget rather than of the slot it stands
			 * in: a button told to take whatever width it is given
			 * is one whose slot says it wants none, and what is
			 * wanted here is the width its words come to */
			QSize hint = item->widget() ? item->widget()->sizeHint() : item->sizeHint();

			if (cellLeast <= 0)
				size.setWidth(qMax(size.width(), hint.width()));

			size.setHeight(qMax(size.height(), hint.height()));
		}

		return size;
	}

	int run(const QRect &outer, bool place) const
	{
		int total = shown();

		if (!total)
			return 0;

		QSize cell = cellSize();
		int gap = spacing();
		int across = qMax(1, (outer.width() + gap) / (qMax(1, cell.width()) + gap));

		across = qMin(across, total);

		if (cellMost > 0)
			across = qMin(across, cellMost);

		int rows = (total + across - 1) / across;

		if (place) {
			int index = 0;

			for (QLayoutItem *item : items) {
				if (item->isEmpty())
					continue;

				int column = index % across;
				int row = index / across;
				/* worked out from the edges rather than from one
				 * width added up, so the last cell ends where
				 * the row does instead of a pixel or two short */
				int left = outer.x() + (outer.width() + gap) * column / across;
				int right = outer.x() + (outer.width() + gap) * (column + 1) / across - gap;

				item->setGeometry(QRect(left, outer.y() + row * (cell.height() + gap),
							qMax(1, right - left), cell.height()));
				index++;
			}
		}

		return rows * cell.height() + (rows - 1) * gap;
	}

	QList<QLayoutItem *> items;
	int cellLeast = 0;
	int cellMost = 0;
};

/* A line of words that says what it can in the room it has: cut to an ellipsis
 * rather than off at the edge, and asking for no width of its own, so a remark
 * in a row of controls is never the thing that holds the dock open. */
class WarpZoomHint : public QLabel {
public:
	explicit WarpZoomHint(const QString &text, QWidget *parent = nullptr) : QLabel(text, parent)
	{
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		setToolTip(text);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);

		painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
		painter.drawText(rect(), (int)alignment(), fontMetrics().elidedText(text(), Qt::ElideRight, width()));
	}
};

/* The name a part of the dock goes under. Said in capitals and in bold so it
 * reads as a heading rather than as one more control, and given up to an
 * ellipsis rather than holding the dock open. */
QLabel *warpZoomCaption(const char *key, QWidget *parent)
{
	auto *label = new QLabel(QString::fromUtf8(obs_module_text(key)).toUpper(), parent);

	label->setStyleSheet(QString::fromUtf8("font-weight: bold;"));
	label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

	return label;
}

/* A button that is one of a set where one is picked - the preset speeds, the
 * zoom stops, direct or confirm. It says which by staying down, takes no
 * keyboard focus mid-show, and gives up its padding rather than the dock's
 * width.
 *
 * It also works out how big it would like to be every time it is asked, rather
 * than once. A push button keeps the answer it gave last, and what it gives
 * while a stylesheet is in the middle of dressing some other widget is the
 * answer of a button with no theme on it at all - the style declines to be asked
 * a second question while it is busy with the first - which is then the answer
 * it goes on giving. The rows these stand in measure them whenever the dock is
 * laid out, which is exactly when a theme is being put on something, so a kept
 * answer is one that is wrong until the theme next changes. */
class WarpZoomChoice : public QPushButton {
public:
	explicit WarpZoomChoice(const QString &text, QWidget *parent = nullptr) : QPushButton(text, parent)
	{
		setCheckable(true);
		setFocusPolicy(Qt::NoFocus);
		setStyleSheet(QString::fromUtf8("padding-left: 8px; padding-right: 8px; min-width: 0px;"));
	}

	QSize sizeHint() const override
	{
		QStyleOptionButton option;

		ensurePolished();
		initStyleOption(&option);

		return style()->sizeFromContents(QStyle::CT_PushButton, &option,
						 fontMetrics().size(Qt::TextShowMnemonic, text()), this);
	}

	QSize minimumSizeHint() const override { return sizeHint(); }
};

QPushButton *warpZoomChoice(const QString &text, QWidget *parent)
{
	return new WarpZoomChoice(text, parent);
}

/* the same, for a set that stands in cells of one size and so is as narrow as
 * the cell it is handed */
QPushButton *warpZoomChip(const QString &text, QWidget *parent)
{
	QPushButton *button = warpZoomChoice(text, parent);

	button->setStyleSheet(QString::fromUtf8("padding-left: 4px; padding-right: 4px; min-width: 0px;"));
	button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

	return button;
}

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
	QSize sizeHint() const override { return QSize(wanted * 16 / 9, wanted); }

	/* The height the picture is worth at the width it has, which the dock
	 * works out and says here: asked for in the hint, so the rows under the
	 * picture start where it ends rather than where a guess at its height
	 * would have put them, and still only a hint, so a dock dragged short
	 * takes it back. */
	void setWanted(int height)
	{
		if (wanted == height)
			return;

		wanted = height;
		updateGeometry();
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

	/* the shape of what is being drawn, which says how tall the picture is
	 * worth making at the width it has; sixteen by nine until there is one */
	QSize shape() const { return picture.isNull() ? QSize(16, 9) : picture.size(); }

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

	/* What the picture is, said on it: what is on air and how far in, and
	 * the shot waiting behind it when there is one. Each stands on a ground
	 * of its own, since words drawn straight onto video are only as easy to
	 * read as whatever happens to be behind them. */
	void drawBadge(QPainter &painter)
	{
		QFont small = font();

		small.setBold(true);
		painter.setFont(small);

		int x = shown.x() + 6;
		int y = shown.y() + 6;

		if (staged)
			x = drawChip(painter, x, y,
				     QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Staged"))
					     .arg(qRound(stage.zoom * 100.0f)),
				     warpZoomStageColor(), QColor(26, 18, 5), false);

		drawChip(painter, x, y,
			 QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.OnAir")).arg(qRound(framing.zoom * 100.0f)),
			 QColor(0, 0, 0, 200), QColor(255, 255, 255), true);

		painter.setFont(font());
	}

	/* One label on the picture, and where the next one starts. A label that
	 * would run off the picture is left off rather than cut: the first one
	 * drawn is the one that matters most. */
	int drawChip(QPainter &painter, int x, int y, const QString &words, const QColor &ground, const QColor &ink,
		     bool tally)
	{
		QFontMetrics metrics = painter.fontMetrics();
		int dot = tally ? metrics.ascent() / 2 + 5 : 0;
		QRect chip(x, y, metrics.horizontalAdvance(words) + dot + 12, metrics.height() + 6);

		if (chip.right() > shown.right() - 4 || chip.bottom() > shown.bottom() - 4)
			return x;

		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(Qt::NoPen);
		painter.setBrush(ground);
		painter.drawRoundedRect(chip, 3, 3);

		if (tally) {
			int size = metrics.ascent() / 2;

			painter.setBrush(QColor(255, 90, 95));
			painter.drawEllipse(QRectF(chip.x() + 6, chip.center().y() - size / 2.0 + 0.5, size, size));
		}

		painter.setRenderHint(QPainter::Antialiasing, false);
		painter.setPen(ink);
		painter.drawText(chip.adjusted(6 + dot, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter, words);

		return chip.right() + 5;
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

		/* big enough to read where there is a picture to spare for it,
		 * and no more of a small one than it was */
		const int size = shown.width() >= 260 ? 72 : 56;
		QRect box(shown.right() - size - 8, shown.bottom() - (size * 9 / 16) - 8, size, size * 9 / 16);

		painter.setPen(QColor(255, 255, 255, 150));
		painter.setBrush(QColor(0, 0, 0, 180));
		painter.drawRect(box);

		/* what is on air, and the shot waiting behind it: with a stage up
		 * the two boxes are what say where the move is going. The waiting
		 * one is dashed as well as amber, so the two are told apart by
		 * more than their colour. */
		drawWindow(painter, box, framing, QPen(QColor(255, 255, 255, 220)), QColor(255, 255, 255, 70));

		if (staged)
			drawWindow(painter, box, stage, QPen(warpZoomStageColor(), 1, Qt::DashLine),
				   QColor(224, 152, 44, 100));
	}

	void drawWindow(QPainter &painter, const QRect &box, const warp_zoom_view &view, const QPen &pen,
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
	int wanted = WARP_ZOOM_PREVIEW_WANT;
};

/* ------------------------------------------------------------------------- */
/* grabbing the picture */

class WarpZoomGrabber {
public:
	~WarpZoomGrabber() { release(); }

	/* What the source is putting out, at something like preview size.
	 * Rendering a source outside the main loop means holding the graphics
	 * context, so the whole grab is done in one pass and the surfaces it
	 * needs are kept between frames.
	 *
	 * Reading a frame back the moment it has been copied out makes the
	 * thread wait on the GPU to finish it, with the graphics context held
	 * and the render thread queued behind it. So the copy goes into one of
	 * two surfaces and the frame read is the one copied out on the grab
	 * before, which the GPU has long since finished: the picture runs a
	 * grab behind, and nothing waits. Only the first grab after the
	 * surfaces are made reads its own frame, so the dock never opens on
	 * nothing. */
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

		/* A frame staged for another source, or long enough ago that it
		 * is no longer what is on air - the dock was put away, or the
		 * grabs stopped - is not one to show, so this grab reads its own. */
		uint64_t now = os_gettime_ns();

		if (source != last_source || now - last_grab > WARP_ZOOM_GRAB_STALE_NS)
			staged = false;

		last_source = source;
		last_grab = now;

		if (!texrender)
			texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

		for (gs_stagesurf_t *&surface : stages) {
			if (!surface)
				surface = gs_stagesurface_create(cx, cy, GS_RGBA);
		}

		gs_texrender_reset(texrender);

		if (texrender && stages[0] && stages[1] && gs_texrender_begin(texrender, cx, cy)) {
			struct vec4 clear;

			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)source_cx, 0.0f, (float)source_cy, -100.0f, 100.0f);

			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

			obs_source_video_render(source);

			gs_blend_state_pop();
			gs_texrender_end(texrender);

			gs_stage_texture(stages[written], gs_texrender_get_texture(texrender));

			/* the frame staged on the grab before, or this one when
			 * there has not been one since the surfaces were made */
			size_t read = staged ? written ^ 1 : written;
			uint8_t *data = nullptr;
			uint32_t linesize = 0;

			if (gs_stagesurface_map(stages[read], &data, &linesize)) {
				image = QImage(data, (int)cx, (int)cy, (int)linesize, QImage::Format_RGBA8888).copy();
				gs_stagesurface_unmap(stages[read]);
			}

			staged = true;
			written ^= 1;
		}

		obs_leave_graphics();

		obs_source_dec_showing(source);

		return image;
	}

	void release()
	{
		if (!texrender && !stages[0] && !stages[1])
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

		for (gs_stagesurf_t *&surface : stages) {
			if (surface) {
				gs_stagesurface_destroy(surface);
				surface = nullptr;
			}
		}

		staged = false;
		written = 0;
	}

	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stages[2] = {nullptr, nullptr};
	/* the surface the next frame is copied into, and whether the other
	 * one holds a frame copied out on an earlier grab */
	size_t written = 0;
	bool staged = false;
	/* only ever compared against, never used to reach the source */
	const obs_source_t *last_source = nullptr;
	uint64_t last_grab = 0;
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

/* A drawing in the theme's colour, at the size it is asked for.
 *
 * The drawings say only a shape. What is kept of one is where it put ink, and
 * the ink itself is thrown away for whatever colour the theme gives the words
 * on a button, so a drawing is as legible on a light theme as on a dark one
 * without there being two of it. Asked for in the screen's own pixels and told
 * what they are worth, so it is sharp on a display that has more of them than
 * it has points. */
QPixmap warpZoomTinted(QSvgRenderer &glyph, int side, const QColor &ink, qreal ratio)
{
	QPixmap sheet(QSize(side, side) * ratio);
	QPainter painter;

	sheet.setDevicePixelRatio(ratio);
	sheet.fill(Qt::transparent);

	painter.begin(&sheet);
	painter.setRenderHint(QPainter::Antialiasing, true);
	glyph.render(&painter, QRectF(0, 0, side, side));
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.fillRect(QRectF(0, 0, side, side), ink);
	painter.end();

	return sheet;
}

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

		sheet = warpZoomTinted(glyph, room, ink, ratio);
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

/* The pad itself: eight directions around the reset dot.
 *
 * The buttons are placed by hand rather than by a grid, because what matters
 * is that they stay square: a grid would hand out whatever space there was and
 * leave the arrows in letterboxes. The pad works out the biggest square that
 * three columns of them fit into, lays itself out at that size, and sits in
 * the middle of whatever it was given. */
class WarpZoomPad : public QWidget {
public:
	explicit WarpZoomPad(QWidget *parent = nullptr) : QWidget(parent)
	{
		/* as wide as its buttons come to and no wider: what is left of
		 * the row is the zoom's */
		QSizePolicy policy(QSizePolicy::Maximum, QSizePolicy::Preferred);

		/* the buttons are square, so the row the pad sits in takes only
		 * the height the width it was given works out to */
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
	}

	void place(QWidget *widget, int row, int column)
	{
		Cell cell;

		widget->setParent(this);

		cell.widget = widget;
		cell.row = row;
		cell.column = column;

		cells.append(cell);
	}

	/* how big a button is allowed to get, which the dock works out from
	 * the room it has */
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

	static int down(int size) { return size * 3 + WARP_ZOOM_PAD_SPACING * 2; }

	static int across(int size) { return size * 3 + WARP_ZOOM_PAD_SPACING * 2; }

	/* The biggest square three rows of them fit down into, and three
	 * columns across - `down' asked the other way round.
	 *
	 * The pad asks for the height the width it was given works out to, and
	 * in a dock with a picture, the presets and the rows under them all
	 * wanting height at once it can be handed less than it asked for. What
	 * it is given is the answer, not what it asked for. */
	static int fitInto(int length) { return (length - WARP_ZOOM_PAD_SPACING * 2) / 3; }

	int sizeFor(int width) const { return qBound(WARP_ZOOM_PAD_MIN, fitInto(width), largest); }

	void arrange()
	{
		/* square: the size the width allows, or the height, whichever
		 * there is less room for */
		int size = qMax(WARP_ZOOM_PAD_FLOOR, qMin(sizeFor(width()), fitInto(height())));
		int left = (width() - across(size)) / 2;
		int top = (height() - down(size)) / 2;

		for (const Cell &cell : cells) {
			/* the square is only handed out if the widget will take
			 * one: whatever the theme has capped it at is put back
			 * first */
			warpZoomFreeSize(cell.widget);

			cell.widget->setGeometry(left + cell.column * (size + WARP_ZOOM_PAD_SPACING),
						 top + cell.row * (size + WARP_ZOOM_PAD_SPACING), size, size);
		}
	}

	QVector<Cell> cells;
	int largest = WARP_ZOOM_PAD_MAX;
};

/* The zoom, as the rocker beside the stick: out on the left, in on the right,
 * and how far in the picture is between them in figures big enough to read
 * from where the operator is actually looking.
 *
 * Laid out by hand for the same reason the pad is - the two buttons are square
 * and a theme has opinions about how big a button may be - with the readout
 * taking whatever the buttons leave. */
class WarpZoomRocker : public QWidget {
public:
	WarpZoomRocker(QPushButton *out, QLabel *readout, QPushButton *in, QWidget *parent = nullptr)
		: QWidget(parent),
		  outButton(out),
		  inButton(in),
		  figures(readout)
	{
		out->setParent(this);
		in->setParent(this);
		readout->setParent(this);

		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	}

	/* how big the two buttons are, which follows the pad's */
	void setSide(int size)
	{
		if (side == size)
			return;

		side = size;
		updateGeometry();
		arrange();
	}

	QSize sizeHint() const override { return QSize(side * 2 + figuresWidth() + 16, side); }

	QSize minimumSizeHint() const override { return QSize(WARP_ZOOM_ROCKER_MIN * 2 + figuresWidth(), side); }

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		arrange();
	}

	void changeEvent(QEvent *event) override
	{
		QWidget::changeEvent(event);

		if (event->type() == QEvent::StyleChange)
			arrange();
	}

private:
	void arrange()
	{
		/* square, and never so big that the figures are squeezed out
		 * from between them */
		int size = qMax(WARP_ZOOM_PAD_FLOOR, qMin(qMin(side, height()), (width() - figuresWidth()) / 2));
		int top = (height() - size) / 2;

		warpZoomFreeSize(outButton);
		warpZoomFreeSize(inButton);

		outButton->setGeometry(0, top, size, size);
		inButton->setGeometry(width() - size, top, size, size);
		figures->setGeometry(size, 0, qMax(0, width() - size * 2), height());
	}

	/* what the readout needs to say the most it ever says */
	int figuresWidth() const
	{
		QString most = QString("%1%").arg(qRound(WARP_ZOOM_MAX * 100.0f));

		return qMax(WARP_ZOOM_READOUT_MIN, figures->fontMetrics().horizontalAdvance(most) + 4);
	}

	QPushButton *outButton;
	QPushButton *inButton;
	QLabel *figures;
	int side = WARP_ZOOM_ROCKER_MAX;
};

/* ------------------------------------------------------------------------- */
/* a preset, as a tile
 *
 * A preset is recalled far more often than it is renamed, so what the dock
 * shows of one is a thing to press rather than a line to pick and a button to
 * press afterwards: one click and the picture is on its way there. The tile
 * says what a list row did - the name, how far in, and the number of the slot
 * hotkey that fires it - and draws where in the picture the framing sits, which
 * a list never could, so two presets at the same zoom are told apart at a
 * glance.
 *
 * Changing a preset is the rare thing and the one that does damage if it is
 * done by accident, so it is not on the tile while the tile recalls. With the
 * dock set to edit the tiles stop recalling and carry the three things that
 * change one instead. */
class WarpZoomTile : public QAbstractButton {
public:
	enum Action { None = -1, Update = 0, Rename = 1, Remove = 2 };

	WarpZoomTile(const WarpZoomPresetRow &row, int index, QWidget *parent = nullptr)
		: QAbstractButton(parent),
		  preset(row),
		  slot(!row.fixed && index >= 1 && index <= WARP_ZOOM_SLOTS ? index : 0),
		  glyphs{QSvgRenderer(warpZoomDrawing("preset-update")), QSvgRenderer(warpZoomDrawing("preset-rename")),
			 QSvgRenderer(warpZoomDrawing("preset-remove"))}
	{
		/* the framing the preset comes out as, which is the one to draw
		 * and the one to compare the picture against */
		warp_zoom_clamp(&preset.view);

		setFocusPolicy(Qt::NoFocus);
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		/* repainted as the cursor comes and goes, so it answers a hover
		 * the way a button does */
		setAttribute(Qt::WA_Hover, true);
		setAccessibleName(row.name);

		QString tip = row.fixed ? QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Reset.Desc"))
					: QString("%1  (%2%)").arg(row.name).arg(qRound(row.view.zoom * 100.0f));

		if (slot)
			tip += QString::fromUtf8("\n") +
			       QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Preset.Slot")).arg(slot);

		setToolTip(tip);

		connect(this, &QAbstractButton::clicked, this, [this]() {
			if (!editing) {
				if (onRecall)
					onRecall();
				return;
			}

			Action action = actionAt(pressed);

			if (action != None && onAction)
				onAction(action);
		});
	}

	std::function<void()> onRecall;
	std::function<void(Action)> onAction;

	const WarpZoomPresetRow &row() const { return preset; }

	/* whether this is where the picture is, and whether it is the shot
	 * waiting to go there */
	void setState(bool on_air, bool is_staged)
	{
		if (onAir == on_air && staged == is_staged)
			return;

		onAir = on_air;
		staged = is_staged;
		update();
	}

	void setEditing(bool value)
	{
		if (editing == value)
			return;

		editing = value;
		update();
	}

	QSize sizeHint() const override { return QSize(WARP_ZOOM_TILE_MIN, tall()); }

	/* a few letters of the name is as little as a tile is worth */
	QSize minimumSizeHint() const override
	{
		return QSize(EDGE * 2 + fontMetrics().averageCharWidth() * 6, tall());
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		pressed = event->position().toPoint();
		QAbstractButton::mousePressEvent(event);
	}

	/* what each of the three says it is, since a drawing this small says
	 * it to nobody who does not already know */
	bool event(QEvent *event) override
	{
		if (event->type() == QEvent::ToolTip && editing && !preset.fixed) {
			auto *help = static_cast<QHelpEvent *>(event);
			static const char *const names[] = {"Warp.Zoom.Dock.Update.Desc", "Warp.Zoom.Dock.Rename",
							    "Warp.Zoom.Dock.Remove"};
			Action action = actionAt(help->pos());

			if (action == None)
				QToolTip::hideText();
			else
				QToolTip::showText(help->globalPos(), QString::fromUtf8(obs_module_text(names[action])),
						   this, actionRect(action));

			return true;
		}

		return QAbstractButton::event(event);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		const QPalette &colours = palette();
		QRectF face = QRectF(rect()).adjusted(1, 1, -1, -1);
		QFont strong = font();

		strong.setBold(true);

		QFontMetrics plain(font());
		QFontMetrics heavy(strong);

		/* On air is the theme's own colour for the thing that is picked,
		 * so it reads the way a selection does everywhere else in OBS;
		 * a tile being edited is set back into the panel, since it is
		 * not a button while it is. */
		QColor ground = colours.color(QPalette::Button);
		QColor ink = colours.color(QPalette::ButtonText);

		if (editing) {
			ground = colours.color(QPalette::Base);
			ink = colours.color(QPalette::Text);
		} else if (onAir) {
			ground = colours.color(QPalette::Highlight);
			ink = colours.color(QPalette::HighlightedText);
		}

		if (!editing && isDown())
			ground = ground.darker(120);
		else if (!editing && underMouse())
			ground = ground.lighter(118);

		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(editing ? QPen(colours.color(QPalette::Button), 1) : QPen(Qt::NoPen));
		painter.setBrush(ground);
		painter.drawRoundedRect(face, 5, 5);

		/* the shot that is waiting: dashed as well as amber, so it is
		 * told from the one on air by more than its colour */
		if (staged && !editing) {
			painter.setPen(QPen(warpZoomStageColor(), 2, Qt::DashLine));
			painter.setBrush(Qt::NoBrush);
			painter.drawRoundedRect(face.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
		}

		int x = EDGE;

		/* the map goes before the name does: a tile too narrow for both
		 * keeps the words */
		if (!editing && width() >= EDGE * 2 + MAP_W + GAP + plain.averageCharWidth() * 8) {
			drawMap(painter);
			x += MAP_W + GAP;
		}

		int room = qMax(0, width() - EDGE - x);
		int top = editing ? EDGE : (height() - (heavy.height() + LEAD + plain.height())) / 2;
		int under = top + heavy.height() + LEAD;

		painter.setFont(strong);
		painter.setPen(ink);
		painter.drawText(QRect(x, top, room, heavy.height()), Qt::AlignLeft | Qt::AlignVCenter,
				 heavy.elidedText(preset.name, Qt::ElideRight, room));
		painter.setFont(font());

		if (editing && !preset.fixed) {
			drawActions(painter);
			return;
		}

		/* the number of the slot hotkey that fires it, on a key of its
		 * own so it is not read as part of the zoom */
		if (slot && !editing) {
			QString key = QString::number(slot);
			QRectF cap(x, under, qMax(plain.height(), plain.horizontalAdvance(key) + 6), plain.height());

			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(0, 0, 0, 90));
			painter.drawRoundedRect(cap, 3, 3);
			painter.setPen(ink);
			painter.drawText(cap, Qt::AlignCenter, key);

			x += (int)cap.width() + 6;
			room = qMax(0, width() - EDGE - x);
		}

		QString said = QString("%1%").arg(qRound(preset.view.zoom * 100.0f));

		if (editing)
			said = QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Preset.Fixed"));
		else if (staged)
			said = QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Preset.Staged"));
		else if (preset.fixed)
			said = QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Preset.Whole"));

		painter.setPen(ink);
		painter.drawText(QRect(x, under, room, plain.height()), Qt::AlignLeft | Qt::AlignVCenter,
				 plain.elidedText(said, Qt::ElideRight, room));
	}

private:
	static constexpr int EDGE = 8;
	static constexpr int GAP = 8;
	static constexpr int LEAD = 5;
	static constexpr int MAP_W = 48;
	static constexpr int MAP_H = 27;

	/* two lines and the room around them, or the buttons a tile carries
	 * while it is being edited, whichever is the taller - worked out the
	 * once so a tile is the same height whichever it is showing */
	int tall() const
	{
		int line = fontMetrics().height();

		return qMax(MAP_H, line + LEAD + line + ACTION_PAD) + EDGE * 2;
	}

	static constexpr int ACTION_PAD = 6;

	QRect actionRect(int which) const
	{
		int high = fontMetrics().height() + ACTION_PAD;
		int wide = qMax(8, qMin(high * 3 / 2, (width() - EDGE * 2 - 8) / 3));

		return QRect(EDGE + which * (wide + 4), height() - EDGE - high, wide, high);
	}

	Action actionAt(const QPoint &point) const
	{
		if (preset.fixed)
			return None;

		for (int which = Update; which <= Remove; which++) {
			if (actionRect(which).contains(point))
				return (Action)which;
		}

		return None;
	}

	void drawActions(QPainter &painter)
	{
		const QPalette &colours = palette();
		QColor ink = colours.color(QPalette::ButtonText);
		QPoint cursor = mapFromGlobal(QCursor::pos());

		for (int which = Update; which <= Remove; which++) {
			QRect box = actionRect(which);
			QColor ground = colours.color(QPalette::Button);
			int side = qMax(1, qRound(box.height() * 0.62));

			if (underMouse() && box.contains(cursor))
				ground = isDown() ? ground.darker(120) : ground.lighter(118);

			painter.setPen(Qt::NoPen);
			painter.setBrush(ground);
			painter.drawRoundedRect(box, 3, 3);
			painter.drawPixmap(QPointF(box.x() + (box.width() - side) / 2.0,
						   box.y() + (box.height() - side) / 2.0),
					   warpZoomTinted(glyphs[which], side, ink, devicePixelRatioF()));
		}
	}

	/* where in the picture the framing sits: the whole of it as a box, and
	 * the part the preset keeps as a block inside */
	void drawMap(QPainter &painter)
	{
		QRectF box(EDGE, (height() - MAP_H) / 2.0, MAP_W, MAP_H);
		float window = 1.0f / preset.view.zoom;
		QRectF inner(box.x() + (preset.view.x - window * 0.5f) * box.width(),
			     box.y() + (preset.view.y - window * 0.5f) * box.height(), window * box.width(),
			     window * box.height());

		painter.setPen(QPen(QColor(138, 144, 160), 1));
		painter.setBrush(QColor(22, 24, 29));
		painter.drawRect(box);

		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(254, 254, 254, 235));
		painter.drawRect(inner.intersected(box.adjusted(0.5, 0.5, -0.5, -0.5)));
	}

	WarpZoomPresetRow preset;
	int slot;
	QSvgRenderer glyphs[3];
	QPoint pressed;
	bool onAir = false;
	bool staged = false;
	bool editing = false;
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
	/* What the dock does with the buttons on it, for a hand already on the
	 * mouse: the picture on or off, direct or confirm, and - over a preset -
	 * everything that can be done to that preset, including the two that
	 * have no button because they are about its place in the list.
	 *
	 * Confirm is a per-source setting - it is the source the dock is pointed
	 * at that is put on confirm, the same one the source properties set - so
	 * it is read back every time the menu is opened rather than remembered
	 * here. */
	void contextMenuEvent(QContextMenuEvent *event) override
	{
		QMenu menu(this);
		WarpZoomTile *tile = tileUnder(event->pos());
		/* taken now rather than read off the tile afterwards: the menu
		 * runs the event loop, and the tiles are remade whenever the
		 * presets change under it */
		QString id = tile ? tile->row().id : QString();
		QString name = tile ? tile->row().name : QString();
		QAction *recallAction = nullptr;
		QAction *updateAction = nullptr;
		QAction *renameAction = nullptr;
		QAction *removeAction = nullptr;
		QAction *upAction = nullptr;
		QAction *downAction = nullptr;

		if (tile) {
			recallAction = menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Recall")));

			if (!tile->row().fixed) {
				updateAction =
					menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Update")));
				renameAction =
					menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Rename")));
				removeAction =
					menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Remove")));
				menu.addSeparator();
				upAction = menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Presets.Up")));
				downAction =
					menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Presets.Down")));

				updateAction->setToolTip(
					QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Update.Desc")));
			}

			menu.addSeparator();
		}

		QAction *full = menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Mode.Full")));
		QAction *small = menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Mode.Minimal")));

		full->setCheckable(true);
		small->setCheckable(true);
		full->setChecked(!minimal);
		small->setChecked(minimal);

		menu.addSeparator();

		QAction *confirmAction = menu.addAction(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Confirm")));

		confirmAction->setCheckable(true);
		confirmAction->setChecked(confirming);
		confirmAction->setEnabled(canConfirm);
		confirmAction->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Confirm.Desc")));

		/* a menu says nothing in a tooltip unless it is asked to */
		menu.setToolTipsVisible(true);

		QAction *picked = menu.exec(event->globalPos());

		if (!picked)
			return;

		if (picked == recallAction) {
			recall(id);
		} else if (picked == updateAction) {
			presetAction(WarpZoomTile::Update, id, name);
		} else if (picked == renameAction) {
			presetAction(WarpZoomTile::Rename, id, name);
		} else if (picked == removeAction) {
			presetAction(WarpZoomTile::Remove, id, name);
		} else if (picked == upAction || picked == downAction) {
			OBSSourceAutoRelease source = zoomSource();

			if (source)
				warp_zoom_source_move_preset(source, WARP_UTF8(id), picked == upAction ? -1 : 1);

			refreshLists();
		} else if (picked == confirmAction) {
			setConfirm(confirmAction->isChecked());
		} else {
			bool wanted = picked == small;

			if (wanted == minimal)
				return;

			setMinimal(wanted);
			warpZoomSetMinimalSetting(minimal);
		}
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
	/* What the dock's room is shared out to: which way round its parts
	 * stand, whether there is room for the picture at all, and then how big
	 * the pad may grow in what is left. */
	void fitParts()
	{
		if (!grid)
			return;

		fitShape();
		fitPreview();
		fitPad();
	}

	/* Whether the parts stand in one column or side by side.
	 *
	 * A dock down the side of the window is a column, and that is how the
	 * parts are stacked: the picture and what is done about a waiting shot,
	 * the framing controls, the presets, the speed. A dock along the bottom
	 * of the window has the width of the window and the height of a strip,
	 * and stacked the same way it has room for the first of those and none
	 * of the rest. So there the parts stand side by side instead - the
	 * picture, then the controls, then the presets - each with the dock's
	 * whole height to itself. */
	void fitShape()
	{
		bool wanted = width() >= WARP_ZOOM_WIDE_MIN && width() * 10 >= height() * 16;

		if (wanted == wide && placed)
			return;

		wide = wanted;
		placed = true;

		for (QWidget *block : {blockTop, blockFrame, blockPresets, blockSpeed})
			grid->removeWidget(block);

		if (wide) {
			grid->addWidget(blockTop, 0, 0, 2, 1);
			grid->addWidget(blockFrame, 0, 1);
			grid->addWidget(blockSpeed, 1, 1, Qt::AlignTop);
			grid->addWidget(blockPresets, 0, 2, 2, 1);
		} else {
			grid->addWidget(blockTop, 0, 0);
			grid->addWidget(blockFrame, 1, 0);
			grid->addWidget(blockPresets, 2, 0);
			grid->addWidget(blockSpeed, 3, 0);
		}

		grid->setColumnStretch(0, wide ? 4 : 1);
		grid->setColumnStretch(1, wide ? 3 : 0);
		grid->setColumnStretch(2, wide ? 4 : 0);
		grid->setRowStretch(1, wide ? 1 : 0);
		grid->setRowStretch(2, wide ? 0 : 1);
		grid->setRowStretch(3, 0);
	}

	/* the height a column of rows holds at a width, leaving out whatever is
	 * hidden and, when it is named, the one thing being placed */
	static int heightOf(QLayout *box, int across, const QWidget *without)
	{
		int total = 0;
		int rows = 0;

		for (int i = 0; i < box->count(); i++) {
			QLayoutItem *item = box->itemAt(i);

			if (item->isEmpty() || (without && item->widget() == without))
				continue;

			total += item->hasHeightForWidth() ? item->minimumHeightForWidth(across)
							   : item->minimumSize().height();
			rows++;
		}

		/* the gaps between them, and the one the thing left out would
		 * stand above if it were put back */
		return total + box->spacing() * (without ? rows : qMax(0, rows - 1));
	}

	/* The height the rest of the dock holds, which is what says whether the
	 * picture has anywhere to go.
	 *
	 * Asked of the rows themselves rather than counted up here, so a theme
	 * with taller buttons or an operator on a bigger font is answered as it
	 * stands rather than against a number written down when this was
	 * written. The picture is left out of it - it is the one being placed -
	 * and so is anything hidden, since a row that is not there holds
	 * nothing. Side by side, the picture only shares its height with the
	 * rows of its own column. */
	int restHeight() const
	{
		QMargins margins = grid->contentsMargins();
		int across = width() - margins.left() - margins.right();
		int total = margins.top() + margins.bottom();

		if (wide)
			return total + heightOf(topBox, blockTop->width() > 0 ? blockTop->width() : across, preview);

		total += heightOf(topBox, across, preview);

		for (QWidget *block : {blockFrame, blockPresets, blockSpeed})
			total += heightOf(block->layout(), across, nullptr) + grid->verticalSpacing();

		/* a row of presets is worth more than a sliver of picture */
		if (!tiles.isEmpty())
			total += tiles.first()->sizeHint().height();

		return total;
	}

	/* Whether the picture is worth the room it would take. Below the floor
	 * it is a strip too thin to frame a shot in, and the pad, the presets
	 * and the speed are what an operator can still work in a dock that
	 * short, so it is put away rather than laid out under them.
	 *
	 * With the room, it is given the height its own shape comes to at the
	 * width it has and no more: a picture handed more than that only draws
	 * bars above and below itself, and that height is the presets'. */
	void fitPreview()
	{
		bool room = !minimal && height() - restHeight() >= WARP_ZOOM_PREVIEW_FLOOR;

		/* asked of what the dock was told rather than of what is on
		 * screen: a child of a panel that has not been shown yet is not
		 * visible whatever the dock has decided about it */
		if (room == preview->isHidden())
			preview->setVisible(room);

		QSize shape = preview->shape();
		int across = blockTop->width() > 0 ? blockTop->width() : width();
		int most = qMax(WARP_ZOOM_PREVIEW_FLOOR, across * shape.height() / qMax(1, shape.width()));

		if (preview->maximumHeight() != most)
			preview->setMaximumHeight(most);

		preview->setWanted(most);

		pictureShape = shape;
	}

	/* How big the pad is let grow, which is a question about the dock's
	 * height as much as its width.
	 *
	 * Across, it may have its share of the row it stands in with the zoom.
	 * Down, it may have a share of the dock: a fifth of a column that also
	 * holds a picture, a quarter of one that does not, and half of a dock
	 * laid out side by side, where the column it is in is its own. A dock
	 * kept short would otherwise be handed a pad taller than the panel it
	 * is in, with the presets squeezed out behind it.
	 *
	 * Narrow enough, and the pad and the zoom stop sharing a row at all. */
	void fitPad()
	{
		bool away = minimal || preview->isHidden();
		int across = blockFrame->width() > 0 ? blockFrame->width() : width();
		bool stacked = across < WARP_ZOOM_STACK_BELOW;
		int share = wide ? height() / 2 : away ? height() / 4 : height() / 5;
		int byHeight = (share - WARP_ZOOM_PAD_SPACING * 2) / 3;
		int byWidth = ((stacked ? across : across * WARP_ZOOM_PAD_SHARE / 100) - WARP_ZOOM_PAD_SPACING * 2) / 3;
		int size = qBound(WARP_ZOOM_PAD_MIN, qMin(byHeight, byWidth),
				  away ? WARP_ZOOM_PAD_MAX_MINIMAL : WARP_ZOOM_PAD_MAX);

		pad->setLargest(size);
		rocker->setSide(qBound(WARP_ZOOM_ROCKER_MIN, size, WARP_ZOOM_ROCKER_MAX));

		QBoxLayout::Direction wanted = stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;

		if (frameRow->direction() != wanted) {
			frameRow->setDirection(wanted);
			frameRow->setAlignment(pad, stacked ? Qt::AlignHCenter : Qt::Alignment());
		}
	}

	/* Minimal drops the picture and nothing else: the pad, the presets and
	 * the speed are what an operator works, and without the picture there
	 * is no per-frame grab to pay for either. The button in the corner of
	 * the dock and the right-click menu are two ways of saying the same
	 * thing, so each is kept in step with the other here. */
	void setMinimal(bool value)
	{
		minimal = value;
		pictureButton->setChecked(!minimal);
		fitParts();
	}

	/* Direct or confirm, which is the source's setting rather than the
	 * dock's: it is asked of the source and read back from it, so the dock
	 * says what is so rather than what it last asked for. */
	void setConfirm(bool confirm)
	{
		OBSSourceAutoRelease source = zoomSource();

		if (source)
			warp_zoom_source_set_confirm(source, confirm);

		refreshView();
	}

	void build()
	{
		grid = new QGridLayout(this);

		/* the dock is meant to go down to a strip beside the picture,
		 * and margins are width like anything else */
		grid->setContentsMargins(4, 4, 4, 4);
		grid->setSpacing(8);

		buildTop();
		buildFrame();
		buildPresets();
		buildSpeed();

		fitParts();
	}

	/* The source, the picture, and what is done about the shot on it. */
	void buildTop()
	{
		blockTop = new QWidget(this);
		topBox = new QVBoxLayout(blockTop);
		topBox->setContentsMargins(0, 0, 0, 0);
		topBox->setSpacing(4);

		/* which source is being framed; the name of a source is longer
		 * than a narrow dock, so the list is let shrink past it and
		 * says the whole of it in the tooltip instead */
		targets = new QComboBox(blockTop);
		targets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		targets->setMinimumContentsLength(4);
		targets->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

		/* What the list is set from when it is left to itself. It is a
		 * setting on the control beside it, so it stands beside it as a
		 * button that stays down rather than as a sentence on a row of
		 * its own; the sentence is what it says when it is asked. */
		follow = warpZoomChoice(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Follow.Short")), blockTop);
		follow->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Follow.Desc")));
		follow->setAccessibleName(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Follow")));

		/* the picture, on or off: what the right-click menu calls Full
		 * and Minimal, where it can be seen and reached */
		pictureButton = new WarpZoomPadButton(warpZoomDrawing("picture"), blockTop);
		pictureButton->setCheckable(true);
		pictureButton->setChecked(true);
		pictureButton->setFocusPolicy(Qt::NoFocus);
		pictureButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		/* a drawing wants a face about as wide as it is tall, not the
		 * width a theme leaves either side of a word */
		pictureButton->setStyleSheet(
			QString::fromUtf8("padding-left: 8px; padding-right: 8px; min-width: 16px;"));
		warpZoomName(pictureButton, "Warp.Zoom.Dock.Picture");

		auto *sourceRow = new QHBoxLayout();

		sourceRow->setSpacing(4);
		sourceRow->addWidget(targets, 1);
		sourceRow->addWidget(follow);
		sourceRow->addWidget(pictureButton);

		preview = new WarpZoomPreview(blockTop);
		preview->onPan = [this](float dx, float dy) {
			pan(dx, dy, 0);
		};
		preview->onZoom = [this](float factor) {
			adjust(factor, -1);
		};
		preview->onHold = [this](bool holding) {
			setRiding(holding);
		};

		/* Lining a shot up before it goes to air.
		 *
		 * Whether a move goes to air or waits to be taken is the first
		 * thing an operator needs to know before touching anything, so
		 * it is said on the dock rather than left in a menu: the pair of
		 * buttons under the picture is the setting and the sign of it.
		 *
		 * Take and Cancel stand in the same row for as long as the
		 * source is on confirm, whether or not a shot is waiting - they
		 * are greyed until one is. Appearing only when a shot was lined
		 * up, they pushed everything under them down a row at the moment
		 * the operator was reaching for it. */
		directButton = warpZoomChoice(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Direct")), blockTop);
		confirmButton =
			warpZoomChoice(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Confirm.Short")), blockTop);

		directButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Direct.Desc")));
		confirmButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Confirm.Desc")));
		directButton->setChecked(true);

		directHint =
			new WarpZoomHint(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Direct.Hint")), blockTop);
		directHint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		takeButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Take")), blockTop);
		dropButton = new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Drop")), blockTop);

		takeButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Take.Desc")));
		takeButton->setVisible(false);
		dropButton->setVisible(false);
		takeButton->setEnabled(false);
		dropButton->setEnabled(false);

		/* Take is what the row is there for, so it has whatever of the
		 * row the rest leaves */
		auto *stageRow = new WarpZoomFlow(4, true);

		stageRow->addWidget(directButton);
		stageRow->addWidget(confirmButton);
		stageRow->addWidget(directHint);
		stageRow->addWidget(dropButton);
		stageRow->addWidget(takeButton);

		topBox->addLayout(sourceRow);
		topBox->addWidget(preview, 1);
		topBox->addLayout(stageRow);
		/* takes up what the picture will not once it is as tall as its
		 * shape allows, so the rows stay together at the top */
		topBox->addStretch(0);

		connect(targets, &QComboBox::currentIndexChanged, this, [this](int index) {
			if (loading || index < 0 || index >= chosen.size())
				return;

			target = chosen[index];
			refreshLists();
		});

		connect(follow, &QPushButton::toggled, this, [this](bool) { refreshLists(); });

		connect(pictureButton, &QPushButton::clicked, this, [this](bool checked) {
			setMinimal(!checked);
			warpZoomSetMinimalSetting(minimal);
		});

		connect(directButton, &QPushButton::clicked, this, [this]() { setConfirm(false); });
		connect(confirmButton, &QPushButton::clicked, this, [this]() { setConfirm(true); });

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
	}

	/* The framing controls: the pad, and the zoom beside it. */
	void buildFrame()
	{
		blockFrame = new QWidget(this);

		auto *box = new QVBoxLayout(blockFrame);

		box->setContentsMargins(0, 0, 0, 0);
		box->setSpacing(4);
		box->addWidget(warpZoomCaption("Warp.Zoom.Dock.Frame", blockFrame));

		/* The pad: eight directions around the reset dot, laid out the
		 * way a PTZ desk is. A corner moves a full step on both axes,
		 * the way a joystick held into its corner covers more ground
		 * than one pushed straight. */
		pad = new WarpZoomPad(blockFrame);

		struct Direction {
			const char *drawing;
			const char *name;
			int column;
			int row;
		};

		static const Direction directions[] = {
			{"pan-up-left", "Warp.Zoom.Dock.Pan.UpLeft", 0, 0},
			{"pan-up", "Warp.Zoom.Dock.Pan.Up", 1, 0},
			{"pan-up-right", "Warp.Zoom.Dock.Pan.UpRight", 2, 0},
			{"pan-left", "Warp.Zoom.Dock.Pan.Left", 0, 1},
			{"pan-right", "Warp.Zoom.Dock.Pan.Right", 2, 1},
			{"pan-down-left", "Warp.Zoom.Dock.Pan.DownLeft", 0, 2},
			{"pan-down", "Warp.Zoom.Dock.Pan.Down", 1, 2},
			{"pan-down-right", "Warp.Zoom.Dock.Pan.DownRight", 2, 2},
		};

		for (const Direction &direction : directions) {
			auto *button = new WarpZoomPadButton(warpZoomDrawing(direction.drawing), pad);
			float dx = (float)(direction.column - 1) * WARP_ZOOM_DOCK_PAN;
			float dy = (float)(direction.row - 1) * WARP_ZOOM_DOCK_PAN;

			/* A drawing says which way a button goes to anyone
			 * looking at it and nothing at all to anyone who is
			 * not, so every one of them says what it is in words,
			 * for the tooltip and for a reader. */
			warpZoomName(button, direction.name);
			warpZoomCompact(button);
			pad->place(button, direction.row, direction.column);

			connect(button, &QPushButton::clicked, this, [this, dx, dy]() { pan(dx, dy, -1); });
		}

		auto *resetButton = new WarpZoomPadButton(warpZoomDrawing("reset"), pad);

		resetButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Reset.Desc")));
		resetButton->setAccessibleName(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Reset")));
		warpZoomCompact(resetButton);
		pad->place(resetButton, 1, 1);

		connect(resetButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();

			if (source)
				warp_zoom_source_reset(source, -1);
		});

		/* The zoom, beside the pad the way the rocker on a camera desk
		 * sits beside the stick: out and in either side of how far in
		 * the picture is, the bar under them, and under that the
		 * doublings as buttons of their own. */
		auto *in = new WarpZoomPadButton(warpZoomDrawing("zoom-in"), blockFrame);
		auto *out = new WarpZoomPadButton(warpZoomDrawing("zoom-out"), blockFrame);

		warpZoomName(in, "Warp.Zoom.Dock.In");
		warpZoomName(out, "Warp.Zoom.Dock.Out");
		warpZoomCompact(in);
		warpZoomCompact(out);

		connect(in, &QPushButton::clicked, this, [this]() { adjust(WARP_ZOOM_STEP, -1); });
		connect(out, &QPushButton::clicked, this, [this]() { adjust(1.0f / WARP_ZOOM_STEP, -1); });

		zoomLabel = new QLabel(blockFrame);
		zoomLabel->setAlignment(Qt::AlignCenter);
		zoomLabel->setMinimumWidth(WARP_ZOOM_READOUT_MIN);
		dressReadout(false);

		rocker = new WarpZoomRocker(out, zoomLabel, in, blockFrame);

		zoomSlider = new QSlider(Qt::Horizontal, blockFrame);
		zoomSlider->setRange(0, WARP_ZOOM_BAR_STEPS);
		zoomSlider->setSingleStep(WARP_ZOOM_BAR_STEPS / 100);
		zoomSlider->setPageStep(WARP_ZOOM_BAR_STEPS / 10);
		zoomSlider->setValue(0);
		zoomSlider->setAccessibleName(QString::fromUtf8(obs_module_text("Warp.Zoom.Zoom")));
		/* a bar has a length it would rather be; in a dock narrowed to a
		 * strip it takes what is left over instead */
		zoomSlider->setMinimumWidth(WARP_ZOOM_SLIDER_MIN);

		auto *stopRow = new WarpZoomCells(0, 0, 2);

		for (float stop : warpZoomStops) {
			QString label = QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Stop")).arg(qRound(stop));
			QPushButton *button = warpZoomChip(label, blockFrame);

			button->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Stop.Desc"))
						   .arg(qRound(stop * 100.0f)));
			button->setAccessibleName(button->toolTip());
			stopRow->addWidget(button);
			stopButtons.append(button);

			connect(button, &QPushButton::clicked, this, [this, stop]() { zoomTo(stop); });
		}

		auto *zoomBox = new QVBoxLayout();

		zoomBox->setSpacing(4);
		zoomBox->addWidget(rocker);
		zoomBox->addStretch(1);
		zoomBox->addWidget(zoomSlider);
		zoomBox->addStretch(1);
		zoomBox->addLayout(stopRow);

		frameRow = new QBoxLayout(QBoxLayout::LeftToRight);
		frameRow->setSpacing(8);
		frameRow->addWidget(pad);
		frameRow->addLayout(zoomBox, 1);

		box->addLayout(frameRow);

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

			view.zoom = warpZoomFromBar(value);

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
	}

	/* The presets: a tile each, and the two buttons that are about the set
	 * rather than about one of them. */
	void buildPresets()
	{
		blockPresets = new QWidget(this);

		auto *box = new QVBoxLayout(blockPresets);

		box->setContentsMargins(0, 0, 0, 0);
		box->setSpacing(4);

		editButton = warpZoomChoice(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Edit")), blockPresets);
		editButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Edit.Desc")));

		auto *saveButton =
			new QPushButton(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.SaveFraming")), blockPresets);

		saveButton->setToolTip(QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Save.Desc")));

		QLabel *caption = warpZoomCaption("Warp.Zoom.Dock.Presets", blockPresets);

		/* the heading has what the buttons leave when they share a line
		 * with it, and a line of its own when they do not */
		caption->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

		auto *head = new WarpZoomFlow(4);

		head->addWidget(caption);
		head->addWidget(editButton);
		head->addWidget(saveButton);

		/* The tiles scroll rather than holding the dock open: a source
		 * can have dozens of presets and the dock has the height it has.
		 * What they stand on is the dock, not a panel of the scroll
		 * area's own, so nothing is drawn behind them. */
		tileHost = new QWidget();
		tileCells = new WarpZoomCells(WARP_ZOOM_TILE_MIN, 0, 4);
		tileHost->setLayout(tileCells);

		tileScroll = new QScrollArea(blockPresets);
		tileScroll->setWidgetResizable(true);
		tileScroll->setFrameShape(QFrame::NoFrame);
		tileScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		tileScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
		tileScroll->setWidget(tileHost);
		tileScroll->viewport()->setAutoFillBackground(false);
		tileHost->setAutoFillBackground(false);

		box->addLayout(head);
		box->addWidget(tileScroll, 1);

		connect(editButton, &QPushButton::toggled, this, [this](bool checked) {
			editing = checked;
			editButton->setText(QString::fromUtf8(
				obs_module_text(editing ? "Warp.Zoom.Dock.Edit.Done" : "Warp.Zoom.Dock.Edit")));

			for (WarpZoomTile *tile : tiles)
				tile->setEditing(editing);
		});

		connect(saveButton, &QPushButton::clicked, this, [this]() {
			OBSSourceAutoRelease source = zoomSource();

			if (!source)
				return;

			warpZoomSavePreset(this, source);
			refreshLists();
		});
	}

	/* The speed, which is the other thing an operator rides live. */
	void buildSpeed()
	{
		blockSpeed = new QWidget(this);

		auto *box = new QVBoxLayout(blockSpeed);

		box->setContentsMargins(0, 0, 0, 0);
		box->setSpacing(4);

		speedLabel = new QLabel(blockSpeed);
		speedLabel->setMinimumWidth(WARP_ZOOM_READOUT_MIN);
		speedLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		speedLabel->setStyleSheet(QString::fromUtf8("font-weight: bold;"));

		auto *head = new QHBoxLayout();

		head->addWidget(warpZoomCaption("Warp.Video.Speed", blockSpeed), 1);
		head->addWidget(speedLabel);

		/* the preset speeds the hotkeys are bound to, the one the source
		 * is playing at staying down */
		auto *speedRow = new WarpZoomCells(0, 0, 2);

		for (int speed : warpZoomSpeeds) {
			QPushButton *button = warpZoomChip(QString("%1%").arg(speed), blockSpeed);

			speedRow->addWidget(button);
			speedButtons.append(button);

			connect(button, &QPushButton::clicked, this, [this, speed]() {
				setSpeed(speed);
				refreshView();
			});
		}

		speedSlider = new QSlider(Qt::Horizontal, blockSpeed);
		speedSlider->setRange(WARP_FLOW_SPEED_MIN, WARP_FLOW_SPEED_MAX);
		speedSlider->setValue(100);
		speedSlider->setMinimumWidth(WARP_ZOOM_SLIDER_MIN);
		speedSlider->setAccessibleName(QString::fromUtf8(obs_module_text("Warp.Video.Speed")));

		box->addLayout(head);
		box->addLayout(speedRow);
		box->addWidget(speedSlider);

		connect(speedSlider, &QSlider::valueChanged, this, [this](int value) {
			if (!loading)
				setSpeed(value);
		});
	}

	obs_source_t *zoomSource() const { return warpZoomTargetSource(target); }

	/* the preset tile at a point in the dock, if that is what is there */
	WarpZoomTile *tileUnder(const QPoint &point) const
	{
		for (QWidget *widget = childAt(point); widget && widget != this; widget = widget->parentWidget()) {
			if (tiles.contains(static_cast<WarpZoomTile *>(widget)))
				return static_cast<WarpZoomTile *>(widget);
		}

		return nullptr;
	}

	/* The figures between the zoom buttons: big enough to be read at a
	 * glance, and in the colour of a waiting shot while they are saying
	 * where one would go rather than where the picture is. Their size is
	 * taken from the dock's font rather than their own, which is the one
	 * this has already made bigger. Dressed only when something changes,
	 * since a stylesheet set is a style change to everything under it. */
	void dressReadout(bool staging)
	{
		if (readoutDressed && readoutStaging == staging)
			return;

		readoutDressed = true;
		readoutStaging = staging;

		QString sheet = QString::fromUtf8("font-weight: bold; font-size: %1px;")
					.arg(qMax(14, fontInfo().pixelSize() * 3 / 2));

		if (staging)
			sheet += QString::fromUtf8(" color: %1;").arg(warpZoomStageColor().name(QColor::HexRgb));

		zoomLabel->setStyleSheet(sheet);
	}

	/* Take, in the colour of the shot it would put up while there is one,
	 * so the button that puts something on air is the one that stands out
	 * when it can. */
	void dressTake(bool armed)
	{
		if (takeDressed && takeArmed == armed)
			return;

		takeDressed = true;
		takeArmed = armed;

		takeButton->setStyleSheet(armed ? QString::fromUtf8("background-color: %1; color: #1a1205;")
							  .arg(warpZoomStageColor().name(QColor::HexRgb))
						: QString());
	}

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
		bool confirm_on = false;
		bool have = warp_zoom_source_stage(source, out, &is_staged, &confirm_on);

		if (staging)
			*staging = have && confirm_on;

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

	void recall(const QString &id)
	{
		OBSSourceAutoRelease source = zoomSource();

		if (source && !id.isEmpty())
			warp_zoom_source_recall(source, WARP_UTF8(id));

		refreshView();
	}

	/* One of the doublings under the bar. A jump rather than a nudge, so it
	 * eases the way a preset recall does, and keeps the middle of the
	 * picture where it is. */
	void zoomTo(float zoom)
	{
		OBSSourceAutoRelease source = zoomSource();
		warp_zoom_view view;

		if (!source || !framingNow(source, &view))
			return;

		view.zoom = zoom;
		warp_zoom_source_set(source, &view, -1);
		refreshView();
	}

	/* the three things that change a preset, by the id it was made with */
	void presetAction(WarpZoomTile::Action action, const QString &id, const QString &name)
	{
		OBSSourceAutoRelease source = zoomSource();

		if (!source || id.isEmpty())
			return;

		if (action == WarpZoomTile::Update) {
			warp_zoom_view view;

			/* Whatever is being looked at: the shot lined up when
			 * there is one, and otherwise the framing it is heading
			 * for, so keeping a shot partway through a move keeps
			 * the shot rather than wherever the move had got to. */
			if (framingNow(source, &view))
				warp_zoom_source_update_preset(source, WARP_UTF8(id), nullptr, &view, -1);
		} else if (action == WarpZoomTile::Rename) {
			bool ok = false;
			QString typed = QInputDialog::getText(
				this, QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Rename")),
				QString::fromUtf8(obs_module_text("Warp.Zoom.Preset.Save.Prompt")), QLineEdit::Normal,
				name, &ok);

			if (ok && !typed.trimmed().isEmpty())
				warp_zoom_source_update_preset(source, WARP_UTF8(id), WARP_UTF8(typed.trimmed()),
							       nullptr, -1);
		} else if (action == WarpZoomTile::Remove) {
			warp_zoom_source_remove_preset(source, WARP_UTF8(id));
		}

		refreshLists();
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
		QStringList said;

		/* everything a tile draws, so the tiles are remade when any of
		 * it changes and left alone - hover, press and all - while the
		 * dock merely ticks over */
		for (const WarpZoomPresetRow &row : rows)
			said << QString("%1|%2|%3|%4|%5|%6")
					.arg(row.id, row.name)
					.arg(row.view.zoom)
					.arg(row.view.x)
					.arg(row.view.y)
					.arg(row.fixed);

		if (said == shownPresets)
			return;

		shownPresets = said;

		/* Put away rather than deleted on the spot: this is reached
		 * from a tile's own click when a preset is removed, and a
		 * widget is not deleted from inside the signal it is sending. */
		for (WarpZoomTile *tile : tiles) {
			tileCells->removeWidget(tile);
			tile->hide();
			tile->deleteLater();
		}

		tiles.clear();

		for (int i = 0; i < rows.size(); i++) {
			auto *tile = new WarpZoomTile(rows[i], i, tileHost);
			QString id = rows[i].id;
			QString name = rows[i].name;

			tile->setEditing(editing);
			tile->onRecall = [this, id]() {
				recall(id);
			};
			/* asked for once the click that asked has finished, as
			 * renaming opens a window and removing remakes the tiles */
			tile->onAction = [this, id, name](WarpZoomTile::Action action) {
				QMetaObject::invokeMethod(
					this, [this, action, id, name]() { presetAction(action, id, name); },
					Qt::QueuedConnection);
			};

			tileCells->addWidget(tile);
			tile->show();
			tiles.append(tile);
		}

		/* the first tile is what says how much room a row of them
		 * takes, which the picture is placed around */
		fitParts();
	}

	void refreshView()
	{
		OBSSourceAutoRelease source = zoomSource();
		warp_zoom_view view = warp_zoom_default_view();
		warp_zoom_view heading = warp_zoom_default_view();
		warp_zoom_view stage = warp_zoom_default_view();
		bool have = source && warp_zoom_source_get(source, &view, &heading);
		bool is_staged = false;
		bool confirm_on = false;

		if (have)
			warp_zoom_source_stage(source, &stage, &is_staged, &confirm_on);

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
			zoomSlider->setValue(warpZoomToBar(shown.zoom));

		zoomLabel->setText(QString("%1%").arg(qRound(shown.zoom * 100.0f)));
		dressReadout(is_staged);

		for (int i = 0; i < stopButtons.size(); i++) {
			stopButtons[i]->setEnabled(have);
			stopButtons[i]->setChecked(have && std::fabs(shown.zoom - warpZoomStops[i]) < 0.005f);
		}

		/* which preset the picture is on, and which one is waiting */
		for (WarpZoomTile *tile : tiles)
			tile->setState(have && warpZoomSameView(heading, tile->row().view),
				       is_staged && warpZoomSameView(stage, tile->row().view));

		/* Direct or confirm, read back from the source every time: it is
		 * the source's setting, and the properties window and a
		 * websocket request can change it as well as the dock. */
		canConfirm = have;
		confirming = confirm_on;

		directButton->setEnabled(have);
		confirmButton->setEnabled(have);
		directButton->setChecked(!confirm_on);
		confirmButton->setChecked(confirm_on);

		directHint->setVisible(!confirm_on);
		takeButton->setVisible(confirm_on);
		dropButton->setVisible(confirm_on);
		takeButton->setEnabled(is_staged);
		dropButton->setEnabled(is_staged);
		/* where taking it would put the picture, on the button that
		 * does it */
		takeButton->setText(is_staged ? QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.TakeTo"))
							.arg(qRound(stage.zoom * 100.0f))
					      : QString::fromUtf8(obs_module_text("Warp.Zoom.Dock.Take")));
		dressTake(is_staged);

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

		for (int i = 0; i < speedButtons.size(); i++) {
			speedButtons[i]->setEnabled(have_speed);
			speedButtons[i]->setChecked(have_speed && speed == warpZoomSpeeds[i]);
		}

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

		/* a source of another shape is a picture of another height */
		if (preview->shape() != pictureShape)
			fitParts();

		refreshView();
	}

	QGridLayout *grid = nullptr;
	QVBoxLayout *topBox = nullptr;
	QBoxLayout *frameRow = nullptr;
	QWidget *blockTop = nullptr;
	QWidget *blockFrame = nullptr;
	QWidget *blockPresets = nullptr;
	QWidget *blockSpeed = nullptr;

	QComboBox *targets = nullptr;
	QPushButton *follow = nullptr;
	WarpZoomPadButton *pictureButton = nullptr;
	WarpZoomPreview *preview = nullptr;
	QPushButton *directButton = nullptr;
	QPushButton *confirmButton = nullptr;
	QLabel *directHint = nullptr;
	QPushButton *takeButton = nullptr;
	QPushButton *dropButton = nullptr;
	WarpZoomPad *pad = nullptr;
	WarpZoomRocker *rocker = nullptr;
	QSlider *zoomSlider = nullptr;
	QLabel *zoomLabel = nullptr;
	QVector<QPushButton *> stopButtons;
	QPushButton *editButton = nullptr;
	QScrollArea *tileScroll = nullptr;
	QWidget *tileHost = nullptr;
	WarpZoomCells *tileCells = nullptr;
	QVector<WarpZoomTile *> tiles;
	QSlider *speedSlider = nullptr;
	QLabel *speedLabel = nullptr;
	QVector<QPushButton *> speedButtons;
	QTimer *previewTimer = nullptr;
	QTimer *refreshTimer = nullptr;

	QVector<WarpZoomTarget> chosen;
	QStringList shownPresets;
	WarpZoomTarget target;
	/* what the dock was holding last time round, so leaving a source can
	 * drop the shot it had waiting */
	WarpZoomTarget held;
	WarpZoomGrabber grabber;
	/* the shape of the picture the height it is given was worked out for */
	QSize pictureShape;
	bool loading = false;
	bool minimal = false;
	bool wide = false;
	bool placed = false;
	bool editing = false;
	/* what the source the dock is pointed at has to say about confirm, for
	 * the menu that is only built when it is asked for */
	bool confirming = false;
	bool canConfirm = false;
	/* what the readout and Take were last dressed as */
	bool readoutDressed = false;
	bool readoutStaging = false;
	bool takeDressed = false;
	bool takeArmed = false;
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
