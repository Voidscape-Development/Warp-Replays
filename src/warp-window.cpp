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

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QDialog>
#include <QMainWindow>
#include <QPointer>

namespace {

QPointer<QDialog> warp_dialog;

void show_warp_window(void *)
{
	if (!warp_dialog) {
		auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

		warp_dialog = new QDialog(main_window);
		warp_dialog->setWindowTitle(obs_module_text("Warp.Window.Title"));
		warp_dialog->resize(800, 500);
	}

	warp_dialog->show();
	warp_dialog->raise();
	warp_dialog->activateWindow();
}

} // namespace

extern "C" void warp_register_tools_menu(void)
{
	obs_frontend_add_tools_menu_item(obs_module_text("Warp.Tools.Menu"), show_warp_window, nullptr);
}
