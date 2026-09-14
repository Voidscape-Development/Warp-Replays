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
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTreeWidget;

/* Everything about a Warp buffer: what it is called, the angles it holds, the
 * lengths it will write, and whether it starts on its own or with OBS's replay
 * buffer.
 *
 * The same dialog makes one and edits one - there is no kind to pick the way
 * there is for a flow, so a new buffer is an empty one of these with two
 * sensible defaults already in it. */
class WarpBufferDialog : public QDialog {
public:
	/* an empty 'bufferId' makes a buffer rather than editing one */
	WarpBufferDialog(QWidget *parent, const QString &bufferId);

	/* the id of the buffer that was made or edited */
	QString bufferId() const { return id; }

	void accept() override;

private:
	void addAngle();
	void removeAngle();
	void moveAngle(int delta);
	void addLength();
	void removeLength();
	void angleSelectionChanged();
	void lengthSelectionChanged();
	/* says what the buffer will hold in memory, from the angles and lengths
	 * in the dialog right now rather than from the buffer as it is saved */
	void refreshEstimate();

	QString id;

	QLineEdit *nameEdit = nullptr;
	QTreeWidget *angleTree = nullptr;
	QComboBox *angleSource = nullptr;
	QLineEdit *angleName = nullptr;
	QPushButton *angleAdd = nullptr;
	QPushButton *angleRemove = nullptr;
	QPushButton *angleUp = nullptr;
	QPushButton *angleDown = nullptr;
	QListWidget *lengthList = nullptr;
	QSpinBox *lengthSpin = nullptr;
	QPushButton *lengthAdd = nullptr;
	QPushButton *lengthRemove = nullptr;
	QCheckBox *followCheck = nullptr;
	QSpinBox *sizeSpin = nullptr;
	QLabel *estimate = nullptr;
};
