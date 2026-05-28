/**************************************************************************/
/*  rtgi_sdk_manager.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "editor/gui/editor_file_dialog.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"

class RTGISDKManager : public ConfirmationDialog {
	GDCLASS(RTGISDKManager, ConfirmationDialog)

	LineEdit *nrd_path = nullptr;
	Button *nrd_path_browse = nullptr;
	Button *nrd_install_project = nullptr;
	EditorFileDialog *browse_dialog = nullptr;
	Label *path_status = nullptr;

	static RTGISDKManager *singleton;

	static String _get_default_project_nrd_path();
	static String _globalize_path(const String &p_path);
	static bool _is_valid_nrd_path(const String &p_path);
	static void _ensure_project_addon_ignore(const String &p_path);
	static String _trim_process_output(const String &p_output);

	void _validate_path(const String &p_path);
	void _select_dir(const String &p_path);
	void _path_confirmed();
	void _browse_install();
	void _install_project_nrd();

protected:
	void _notification(int p_what);

public:
	static RTGISDKManager *get_singleton() { return singleton; }

	void show_dialog();

	RTGISDKManager();
};
