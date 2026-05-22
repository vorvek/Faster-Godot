def can_build(env, platform):
    if env["disable_physics_2d"]:
        return False

    # Rapier doesn't support double precision
    if env.get("precision", "single") == "double":
        return False

    supported_arches = {
        "linuxbsd": ["x86_64", "arm64"],
        "macos": ["x86_64", "arm64"],
        "windows": ["x86_64", "arm64"],
    }
    return env["platform"] in supported_arches and env["arch"] in supported_arches[env["platform"]]


def configure(env):
    pass


def get_doc_classes():
    return [
        "Fluid2D",
        "FluidEffect2D",
        "FluidEffect2DElasticity",
        "FluidEffect2DSurfaceTensionAKINCI",
        "FluidEffect2DSurfaceTensionHE",
        "FluidEffect2DSurfaceTensionWCSPH",
        "FluidEffect2DViscosityArtificial",
        "FluidEffect2DViscosityDFSPH",
        "FluidEffect2DViscosityXSPH",
        "RapierDirectBodyState2D",
        "RapierDirectSpaceState2D",
        "RapierMath2D",
        "RapierPhysics2DExtensionLibrary",
        "RapierPhysicsServer2D",
        "RapierPhysicsServerFactory2D",
    ]


def get_doc_path():
    return "doc_classes"
