#include "register_types.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/extension/gdextension_interface.gen.h"
#include "core/extension/gdextension_loader.h"
#include "core/extension/gdextension_manager.h"
#include "servers/physics_2d/physics_server_2d.h"

static constexpr const char *RAPIER_2D_EXTENSION_NAME = "rapier_2d";
static constexpr const char *RAPIER_2D_SERVER_NAME = "Rapier2D";
static constexpr const char *RAPIER_2D_PROJECT_EXTENSION_PATH = "res://addons/godot-rapier2d/godot-rapier2d.gdextension";
static constexpr const char *RAPIER_2D_LOAD_BUILTIN_SETTING = "faster_godot/physics_2d/load_builtin_rapier";

class GDExtensionStaticLibraryLoader : public GDExtensionLoader {
	friend class GDExtensionManager;
	friend class GDExtension;

private:
	void *entry_funcptr = nullptr;
	String library_path;

public:
	void set_entry_funcptr(void *p_entry_funcptr) {
		entry_funcptr = p_entry_funcptr;
	}
	virtual Error open_library(const String &p_path) override {
		library_path = p_path;
		return OK;
	}
	virtual Error
	initialize(GDExtensionInterfaceGetProcAddress p_get_proc_address,
			const Ref<GDExtension> &p_extension,
			GDExtensionInitialization *r_initialization) override {
		GDExtensionInitializationFunction initialization_function =
				(GDExtensionInitializationFunction)entry_funcptr;
		if (initialization_function == nullptr) {
			ERR_PRINT("GDExtension initialization function '" + library_path +
					"' is null.");
			return FAILED;
		}
		GDExtensionBool ret = initialization_function(
				p_get_proc_address, p_extension.ptr(), r_initialization);

		if (ret) {
			return OK;
		} else {
			ERR_PRINT("GDExtension initialization function '" + library_path +
					"' returned an error.");
			return FAILED;
		}
	}
	virtual void close_library() override {}
	virtual bool is_library_open() const override { return true; }
	virtual bool has_library_changed() const override { return false; }
	virtual bool library_exists() const override { return true; }
};

extern "C" {
GDExtensionBool
rapier_2d_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization);
}

static bool is_project_rapier_2d_extension_loaded() {
	GDExtensionManager *manager = GDExtensionManager::get_singleton();
	if (manager == nullptr) {
		return false;
	}

	if (manager->is_extension_loaded(RAPIER_2D_PROJECT_EXTENSION_PATH)) {
		return true;
	}

	Vector<String> loaded_extensions = manager->get_loaded_extensions();
	for (const String &extension_path : loaded_extensions) {
		const String lower_path = extension_path.to_lower();
		if (lower_path.find("godot-rapier2d") != -1 || lower_path.find("rapier2d") != -1) {
			return true;
		}
	}

	return false;
}

void initialize_rapier_2d_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		if (Engine::get_singleton()->is_project_manager_hint()) {
			return;
		}

		const bool load_builtin_rapier = GLOBAL_DEF_BASIC(RAPIER_2D_LOAD_BUILTIN_SETTING, true);
		ProjectSettings::get_singleton()->set_restart_if_changed(RAPIER_2D_LOAD_BUILTIN_SETTING, true);
		if (!load_builtin_rapier) {
			return;
		}

		// Projects that already loaded the Rapier GDExtension should keep using
		// their project-local copy to avoid duplicate Rapier2D server registration.
		if (is_project_rapier_2d_extension_loaded()) {
			return;
		}

		Ref<GDExtensionStaticLibraryLoader> loader;
		loader.instantiate();
		loader->set_entry_funcptr((void *)&rapier_2d_init);
		GDExtensionManager::get_singleton()->load_extension_with_loader(RAPIER_2D_EXTENSION_NAME, loader);
		return;
	}

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

#ifdef FASTER_GODOT
	PhysicsServer2DManager *manager = PhysicsServer2DManager::get_singleton();
	if (manager != nullptr && manager->find_server_id(RAPIER_2D_SERVER_NAME) != -1) {
		manager->set_default_server(RAPIER_2D_SERVER_NAME, 10);
	}
#endif
}

void uninitialize_rapier_2d_module(ModuleInitializationLevel p_level) {
	// Nothing to do here
}
