use crate::math::Real;
use crate::object::{BoundaryHandle, FluidHandle};
use na::Vector3;
use rapier_testbed::{egui, harness::Harness, GraphicsManager, PhysicsState, TestbedPlugin};
use kiss3d::window::Window;

use crate::integrations::rapier::FluidsPipeline;
use std::collections::HashMap;

pub const FLUIDS_RENDERING_MAP: [(&str, FluidsRenderingMode); 3] = [
    ("Static", FluidsRenderingMode::StaticColor),
    (
        "Velocity Color",
        FluidsRenderingMode::VelocityColor {
            min: 0.0,
            max: 50.0,
        },
    ),
    (
        "Velocity Arrows",
        FluidsRenderingMode::VelocityArrows {
            min: 0.0,
            max: 50.0,
        },
    ),
];

/// How the fluids should be rendered by the testbed.
#[derive(Copy, Clone, Debug, PartialEq)]
pub enum FluidsRenderingMode {
    /// Use a plain color.
    StaticColor,
    /// Use a red taint the closer to `max` the velocity is.
    VelocityColor {
        /// Fluids with a velocity smaller than this will not have any red taint.
        min: Real,
        /// Fluids with a velocity greater than this will be completely red.
        max: Real,
    },
    /// Show particles as arrows indicating the velocity.
    VelocityArrows {
        /// Fluids with a velocity smaller than this will not have any red taint.
        min: Real,
        /// Fluids with a velocity greater than this will be completely red.
        max: Real,
    },
}

/// A user-defined callback executed at each frame.
pub type FluidCallback = Box<dyn FnMut(&mut Harness, &mut FluidsPipeline)>;

/// A plugin for stepping fluids inside the Rapier testbed.
pub struct FluidsTestbedPlugin {
    /// Whether to render the boundary particles.
    pub render_boundary_particles: bool,
    /// Rendering mode of fluid particles.
    pub fluids_rendering_mode: FluidsRenderingMode,
    callbacks: Vec<FluidCallback>,
    step_time: f64,
    fluids_pipeline: FluidsPipeline,
    #[allow(dead_code)]
    f2color: HashMap<FluidHandle, Vector3<Real>>,
    #[allow(dead_code)]
    boundary2color: HashMap<BoundaryHandle, Vector3<Real>>,
    #[allow(dead_code)]
    default_fluid_color: Vector3<Real>,
}

impl FluidsTestbedPlugin {
    /// Initializes the plugin.
    pub fn new() -> Self {
        Self {
            render_boundary_particles: false,
            fluids_rendering_mode: FluidsRenderingMode::StaticColor,
            step_time: 0.0,
            callbacks: Vec::new(),
            fluids_pipeline: FluidsPipeline::new(0.025, 2.0),
            f2color: HashMap::new(),
            boundary2color: HashMap::new(),
            default_fluid_color: Vector3::new(0.0, 0.0, 0.5),
        }
    }

    /// Adds a callback to be executed at each frame.
    pub fn add_callback(&mut self, f: impl FnMut(&mut Harness, &mut FluidsPipeline) + 'static) {
        self.callbacks.push(Box::new(f))
    }

    /// Sets the fluids pipeline used by the testbed.
    pub fn set_pipeline(&mut self, fluids_pipeline: FluidsPipeline) {
        self.fluids_pipeline = fluids_pipeline;
        self.fluids_pipeline.liquid_world.counters.enable();
    }

    /// Sets the color used to render the specified fluid.
    pub fn set_fluid_color(&mut self, fluid: FluidHandle, color: Vector3<Real>) {
        let _ = self.f2color.insert(fluid, color);
    }

    /// Sets the way fluids are rendered.
    pub fn set_fluid_rendering_mode(&mut self, mode: FluidsRenderingMode) {
        self.fluids_rendering_mode = mode;
    }

    /// Enables the rendering of boundary particles.
    pub fn enable_boundary_particles_rendering(&mut self, enabled: bool) {
        self.render_boundary_particles = enabled;
    }
}

impl TestbedPlugin for FluidsTestbedPlugin {
    fn init_plugin(&mut self) {}

    fn init_graphics(
        &mut self,
        _graphics: &mut GraphicsManager,
        _window: &mut Window,
        _harness: &mut Harness,
    ) {
    }

    fn clear_graphics(&mut self, _graphics: &mut GraphicsManager, _window: &mut Window) {}

    fn run_callbacks(&mut self, harness: &mut Harness) {
        for f in &mut self.callbacks {
            f(harness, &mut self.fluids_pipeline)
        }
    }

    fn step(&mut self, physics: &mut PhysicsState) {
        let dt = physics.integration_parameters.dt;
        self.fluids_pipeline.step(
            &physics.gravity,
            dt,
            &physics.colliders,
            &mut physics.bodies,
        );
    }

    fn draw(
        &mut self,
        _graphics: &mut GraphicsManager,
        _window: &mut Window,
        _harness: &mut Harness,
    ) {
    }

    fn update_ui(
        &mut self,
        _ui_context: &egui::Context,
        _harness: &mut Harness,
        _graphics: &mut GraphicsManager,
        _window: &mut Window,
    ) {
    }

    fn profiling_string(&self) -> String {
        format!("Fluids: {:.2}ms", self.step_time)
    }
}
