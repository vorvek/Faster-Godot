/**************************************************************************/
/*  rtgi_sdk_manager.cpp                                                  */
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

#include "rtgi_sdk_manager.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/link_button.h"
#include "servers/text/text_server.h"

static const char *RTGI_NRD_EDITOR_SETTING = "filesystem/tools/rtgi/nrd_sdk_path";
static const char *RTGI_NRD_PROJECT_SETTING = "rendering/path_tracing/nvidia/nrd_sdk_path";
static const char *RTGI_NRD_DEFAULT_PROJECT_PATH = "res://addons/rtgi_vendor_sdks/nrd";
static const char *RTGI_NRD_GIT_URL = "https://github.com/NVIDIA-RTX/NRD.git";

String RTGISDKManager::_get_default_project_nrd_path() {
	const Variant configured = ProjectSettings::get_singleton()->get_setting(RTGI_NRD_PROJECT_SETTING, RTGI_NRD_DEFAULT_PROJECT_PATH);
	String path = configured.get_type() == Variant::STRING ? String(configured) : String(RTGI_NRD_DEFAULT_PROJECT_PATH);
	if (path.is_empty()) {
		path = RTGI_NRD_DEFAULT_PROJECT_PATH;
	}
	return _globalize_path(path);
}

String RTGISDKManager::_globalize_path(const String &p_path) {
	return ProjectSettings::get_singleton()->globalize_path(p_path.strip_edges()).simplify_path();
}

bool RTGISDKManager::_is_valid_nrd_path(const String &p_path) {
	if (p_path.is_empty()) {
		return false;
	}
	const String root = _globalize_path(p_path);
	return FileAccess::exists(root.path_join("Include").path_join("NRD.h")) &&
			FileAccess::exists(root.path_join("Include").path_join("NRDDescs.h")) &&
			DirAccess::dir_exists_absolute(root.path_join("Shaders"));
}

void RTGISDKManager::_ensure_project_addon_ignore(const String &p_path) {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (project_settings == nullptr) {
		return;
	}

	const String localized_path = project_settings->localize_path(_globalize_path(p_path));
	if (!localized_path.begins_with("res://addons/")) {
		return;
	}

	const String ignore_path = _globalize_path(p_path).path_join(".gdignore");
	if (FileAccess::exists(ignore_path)) {
		return;
	}

	Ref<FileAccess> file = FileAccess::open(ignore_path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_line("# The NRD SDK checkout is kept for native RTGI builds and should not be imported as project assets.");
	}
}

String RTGISDKManager::_trim_process_output(const String &p_output) {
	String output = p_output.strip_edges();
	if (output.length() > 1200) {
		output = output.substr(output.length() - 1200, 1200);
	}
	return output;
}

void RTGISDKManager::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			nrd_path_browse->set_button_icon(get_editor_theme_icon(SNAME("FolderBrowse")));
			nrd_install_project->set_button_icon(get_editor_theme_icon(SNAME("AssetLib")));
		} break;

		case NOTIFICATION_READY: {
			connect(SceneStringName(confirmed), callable_mp(this, &RTGISDKManager::_path_confirmed));
		} break;
	}
}

void RTGISDKManager::show_dialog() {
	String configured_path = EDITOR_GET(RTGI_NRD_EDITOR_SETTING);
	if (configured_path.is_empty()) {
		configured_path = _get_default_project_nrd_path();
	}
	nrd_path->set_text(configured_path);
	_validate_path(configured_path);
	popup_centered();
}

void RTGISDKManager::_validate_path(const String &p_path) {
	const String path = _globalize_path(p_path);
	String status;
	bool success = false;

	if (path.is_empty()) {
		status = TTR("Path to the NRD SDK is empty.");
	} else if (!_is_valid_nrd_path(path)) {
		status = TTR("Path is not a valid NRD SDK checkout. Expected Include/NRD.h, Include/NRDDescs.h, and Shaders/.");
	} else {
		status = TTR("NRD SDK path is valid.");
		success = true;
	}

	if (success) {
		path_status->set_text(status);
		path_status->add_theme_color_override(SceneStringName(font_color), path_status->get_theme_color(SNAME("success_color"), EditorStringName(Editor)));
		get_ok_button()->set_disabled(false);
	} else {
		path_status->set_text(status);
		path_status->add_theme_color_override(SceneStringName(font_color), path_status->get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		get_ok_button()->set_disabled(true);
	}
}

void RTGISDKManager::_select_dir(const String &p_path) {
	nrd_path->set_text(p_path);
	_validate_path(p_path);
}

void RTGISDKManager::_path_confirmed() {
	const String path = _globalize_path(nrd_path->get_text());
	if (!_is_valid_nrd_path(path)) {
		_validate_path(path);
		return;
	}

	const String localized_path = ProjectSettings::get_singleton()->localize_path(path);
	ProjectSettings::get_singleton()->set(RTGI_NRD_PROJECT_SETTING, localized_path.begins_with("res://") ? localized_path : path);
	ProjectSettings::get_singleton()->save();

	EditorSettings::get_singleton()->set(RTGI_NRD_EDITOR_SETTING, path);
	EditorSettings::get_singleton()->save();

	_ensure_project_addon_ignore(path);
	OS::get_singleton()->set_environment("NRD_SDK_PATH", path);
}

void RTGISDKManager::_browse_install() {
	const String path = nrd_path->get_text();
	if (!path.is_empty()) {
		browse_dialog->set_current_dir(_globalize_path(path));
	}
	browse_dialog->popup_centered_ratio();
}

void RTGISDKManager::_install_project_nrd() {
	const String target_path = _get_default_project_nrd_path();
	nrd_path->set_text(target_path);

	String output;
	int exitcode = -1;
	Error err = OK;

	if (_is_valid_nrd_path(target_path)) {
		if (DirAccess::dir_exists_absolute(target_path.path_join(".git"))) {
			List<String> args;
			args.push_back("-C");
			args.push_back(target_path);
			args.push_back("pull");
			args.push_back("--ff-only");
			err = OS::get_singleton()->execute("git", args, &output, &exitcode, true);
		}
	} else if (DirAccess::dir_exists_absolute(target_path)) {
		path_status->set_text(TTR("The project NRD SDK directory already exists but is not an NRD checkout. Choose a different path or remove it before installing."));
		path_status->add_theme_color_override(SceneStringName(font_color), path_status->get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		get_ok_button()->set_disabled(true);
		return;
	} else {
		const Error mkdir_err = DirAccess::make_dir_recursive_absolute(target_path.get_base_dir());
		if (mkdir_err != OK) {
			path_status->set_text(TTR("Could not create the project add-ons directory for the NRD SDK."));
			path_status->add_theme_color_override(SceneStringName(font_color), path_status->get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
			get_ok_button()->set_disabled(true);
			return;
		}

		List<String> args;
		args.push_back("clone");
		args.push_back("--depth");
		args.push_back("1");
		args.push_back(RTGI_NRD_GIT_URL);
		args.push_back(target_path);
		err = OS::get_singleton()->execute("git", args, &output, &exitcode, true);
	}

	if (err != OK || exitcode != 0) {
		String status = TTR("Could not download the NRD SDK. Make sure Git is installed and available on PATH.");
		const String trimmed_output = _trim_process_output(output);
		if (!trimmed_output.is_empty()) {
			status += "\n" + trimmed_output;
		}
		path_status->set_text(status);
		path_status->add_theme_color_override(SceneStringName(font_color), path_status->get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		get_ok_button()->set_disabled(true);
		return;
	}

	if (!_is_valid_nrd_path(target_path)) {
		path_status->set_text(TTR("NRD SDK download finished, but the expected SDK files were not found."));
		path_status->add_theme_color_override(SceneStringName(font_color), path_status->get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		get_ok_button()->set_disabled(true);
		return;
	}

	_path_confirmed();
	_validate_path(target_path);
}

RTGISDKManager *RTGISDKManager::singleton = nullptr;

RTGISDKManager::RTGISDKManager() {
	singleton = this;

	set_title(TTR("Configure RTGI Vendor SDKs"));

	VBoxContainer *vb = memnew(VBoxContainer);
	vb->add_child(memnew(Label(TTR("NVIDIA NRD is required for the optional RTXPT denoiser path. The SDK is not bundled with the engine; install it into the project add-ons folder or provide an existing checkout."))));

	LinkButton *nrd_link = memnew(LinkButton);
	nrd_link->set_text(TTR("Open NVIDIA NRD on GitHub"));
	nrd_link->set_uri(RTGI_NRD_GIT_URL);
	vb->add_child(nrd_link);

	HBoxContainer *hb = memnew(HBoxContainer);

	nrd_path = memnew(LineEdit);
	nrd_path->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	nrd_path->set_accessibility_name(TTRC("NRD SDK Path"));
	hb->add_child(nrd_path);

	nrd_path_browse = memnew(Button);
	nrd_path_browse->set_text(TTR("Browse"));
	nrd_path_browse->connect(SceneStringName(pressed), callable_mp(this, &RTGISDKManager::_browse_install));
	hb->add_child(nrd_path_browse);

	nrd_install_project = memnew(Button);
	nrd_install_project->set_text(TTR("Install to Project"));
	nrd_install_project->connect(SceneStringName(pressed), callable_mp(this, &RTGISDKManager::_install_project_nrd));
	hb->add_child(nrd_install_project);

	hb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hb->set_custom_minimum_size(Size2(560 * EDSCALE, 0));
	vb->add_child(hb);

	path_status = memnew(Label);
	path_status->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	path_status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	vb->add_child(path_status);

	add_child(vb);

	nrd_path->connect(SceneStringName(text_changed), callable_mp(this, &RTGISDKManager::_validate_path));

	get_ok_button()->set_text(TTR("Confirm Path"));

	browse_dialog = memnew(EditorFileDialog);
	browse_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	browse_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
	browse_dialog->connect("dir_selected", callable_mp(this, &RTGISDKManager::_select_dir));

	add_child(browse_dialog);
}
