use crate::counters::Counters;
use crate::coupling::CouplingManager;
use crate::geometry::{self, ContactManager, HGrid, HGridEntry};
use crate::math::{Real, Vector};
use crate::object::{Boundary, BoundaryHandle, BoundarySet};
use crate::object::{Fluid, FluidHandle, FluidSet};
use crate::solver::PressureSolver;
use crate::TimestepManager;
#[cfg(feature = "parry")]
use {
    crate::math::Isometry,
    crate::object::ParticleId,
    parry::{bounding_volume::Aabb, query::PointQuery, shape::Shape},
};

/// The physics world for simulating fluids with boundaries.
pub struct LiquidWorld {
    /// Performance counters of the whole fluid simulation engine.
    pub counters: Counters,
    nsubsteps_since_sort: usize,
    particle_radius: Real,
    h: Real,
    fluids: FluidSet,
    boundaries: BoundarySet,
    solver: Box<dyn PressureSolver + Send + Sync>,
    contact_manager: ContactManager,
    timestep_manager: TimestepManager,
    hgrid: HGrid<HGridEntry>,
    /// Coefficient applied when transmitting forces from fluids to coupled rigid bodies.
    /// A value of 1.0 applies full force, 0.5 applies half force, etc.
    /// Default is 1.0.
    pub boundary_force_coefficient: Real,
}

impl LiquidWorld {
    /// Initialize a new liquid world.
    ///
    /// # Parameters
    ///
    /// - `particle_radius`: the radius of every particle on this world.
    /// - `smoothing_factor`: the smoothing factor used to compute the SPH kernel radius.
    ///    The kernel radius will be computed as `particle_radius * smoothing_factor * 2.0.
    /// - `boundary_force_coefficient`: coefficient applied when transmitting forces from fluids to boundaries.
    ///    Default is 1.0 (full force). Use lower values (e.g., 0.5) to reduce fluid influence on rigid bodies.
    pub fn new(
        solver: impl PressureSolver + Send + Sync + 'static,
        particle_radius: Real,
        smoothing_factor: Real,
        boundary_force_coefficient: Real,
    ) -> Self {
        let h = particle_radius * smoothing_factor * na::convert::<_, Real>(2.0);
        Self {
            counters: Counters::new(),
            nsubsteps_since_sort: 0,
            particle_radius,
            h,
            fluids: FluidSet::new(),
            boundaries: BoundarySet::new(),
            solver: Box::new(solver),
            contact_manager: ContactManager::new(),
            timestep_manager: TimestepManager::new(particle_radius),
            hgrid: HGrid::new(h),
            boundary_force_coefficient,
        }
    }

    /// Advances the simulation by `dt` seconds.
    ///
    /// All the fluid particles will be affected by an acceleration equal to `gravity`.
    pub fn step(&mut self, dt: Real, gravity: &Vector<Real>) {
        self.step_with_coupling(dt, gravity, &mut ())
    }

    /// Advances the simulation by `dt` seconds, taking into account coupling with an external rigid-body engine.
    pub fn step_with_coupling(
        &mut self,
        dt: Real,
        gravity: &Vector<Real>,
        coupling: &mut impl CouplingManager,
    ) {
        self.counters.reset();
        self.counters.step_time.start();
        self.timestep_manager.reset(dt);

        self.solver.init_with_fluids(self.fluids.as_slice());

        for fluid in self.fluids.as_mut_slice() {
            fluid.apply_particles_removal();
        }

        // Perform substeps.
        while !self.timestep_manager.is_done() {
            self.nsubsteps_since_sort += 1;
            self.counters.nsubsteps += 1;

            self.counters.stages.collision_detection_time.resume();
            self.counters.cd.grid_insertion_time.resume();
            self.hgrid.clear();
            geometry::insert_fluids_to_grid(self.fluids.as_slice(), &mut self.hgrid);
            self.counters.cd.grid_insertion_time.pause();

            self.counters.cd.boundary_update_time.resume();
            coupling.update_boundaries(
                &self.timestep_manager,
                self.h,
                self.particle_radius,
                &self.hgrid,
                self.fluids.as_mut_slice(),
                &mut self.boundaries,
            );
            self.counters.cd.boundary_update_time.pause();

            self.counters.cd.grid_insertion_time.resume();
            geometry::insert_boundaries_to_grid(self.boundaries.as_slice(), &mut self.hgrid);
            self.counters.cd.grid_insertion_time.pause();

            self.solver.init_with_boundaries(self.boundaries.as_slice());

            self.contact_manager.update_contacts(
                &mut self.counters,
                self.h,
                self.fluids.as_slice(),
                self.boundaries.as_slice(),
                &self.hgrid,
            );

            self.counters.cd.ncontacts = self.contact_manager.ncontacts();
            self.counters.stages.collision_detection_time.pause();

            self.counters.stages.solver_time.resume();
            self.solver.evaluate_kernels(
                self.h,
                &mut self.contact_manager,
                self.fluids.as_slice(),
                self.boundaries.as_slice(),
            );

            self.solver.compute_densities(
                &self.contact_manager,
                self.fluids.as_slice(),
                self.boundaries.as_mut_slice(),
            );

            self.solver.step(
                &mut self.counters,
                &mut self.timestep_manager,
                gravity,
                &mut self.contact_manager,
                self.h,
                self.fluids.as_mut_slice(),
                self.boundaries.as_slice(),
            );

            coupling.transmit_forces(&self.timestep_manager, &self.boundaries, self.boundary_force_coefficient);
            self.counters.stages.solver_time.pause();
        }

        //        if self.nsubsteps_since_sort >= 100 {
        //            self.nsubsteps_since_sort = 0;
        //            println!("Performing z-sort of particles.");
        //            par_iter_mut!(self.fluids.as_mut_slice()).for_each(|fluid| fluid.z_sort())
        //        }

        self.counters.step_time.pause();
        //        println!("Counters: {}", self.counters);
    }

    /// Add a fluid to the liquid world.
    pub fn add_fluid(&mut self, fluid: Fluid) -> FluidHandle {
        self.fluids.insert(fluid)
    }

    /// Add a boundary to the liquid world.
    pub fn add_boundary(&mut self, boundary: Boundary) -> BoundaryHandle {
        self.boundaries.insert(boundary)
    }

    /// Add a fluid to the liquid world.
    pub fn remove_fluid(&mut self, handle: FluidHandle) -> Option<Fluid> {
        self.fluids.remove(handle)
    }

    /// Add a boundary to the liquid world.
    pub fn remove_boundary(&mut self, handle: BoundaryHandle) -> Option<Boundary> {
        self.boundaries.remove(handle)
    }

    /// The set of fluids on this liquid world.
    pub fn fluids(&self) -> &FluidSet {
        &self.fluids
    }

    /// The mutable set of fluids on this liquid world.
    pub fn fluids_mut(&mut self) -> &mut FluidSet {
        &mut self.fluids
    }

    /// The set of boundaries on this liquid world.
    pub fn boundaries(&self) -> &BoundarySet {
        &self.boundaries
    }

    /// The mutable set of boundaries on this liquid world.
    pub fn boundaries_mut(&mut self) -> &mut BoundarySet {
        &mut self.boundaries
    }

    /// The SPH kernel radius.
    pub fn h(&self) -> Real {
        self.h
    }

    /// The radius of every particle on this liquid world.
    pub fn particle_radius(&self) -> Real {
        self.particle_radius
    }

    /// The set of particles potentially intersecting the given AABB.
    #[cfg(feature = "parry")]
    pub fn particles_intersecting_aabb<'a>(
        &'a self,
        aabb: Aabb,
    ) -> impl Iterator<Item = ParticleId> + 'a {
        let mins = aabb.mins.into();
        let maxs = aabb.maxs.into();
        self.hgrid
            .cells_intersecting_aabb(&mins, &maxs)
            .flat_map(|e| e.1)
            .filter_map(move |entry| match entry {
                HGridEntry::FluidParticle(fid, pid) => {
                    let (fluid, handle) = self.fluids.get_from_contiguous_index(*fid)?;

                    // Defensive bounds check for fluids
                    if *pid >= fluid.positions.len() {
                        return None;
                    }

                    let pt = fluid.positions[*pid];

                    if aabb.distance_to_local_point(pt.into(), true) < self.particle_radius {
                        Some(ParticleId::FluidParticle(handle, *pid))
                    } else {
                        None
                    }
                }
                HGridEntry::BoundaryParticle(bid, pid) => {
                    let (boundary, handle) = self.boundaries.get_from_contiguous_index(*bid)?;
                    
                    // --- DEFENSIVE FIX ---
                    // Add a bounds check. This handles the race condition where
                    // a boundary's particles are cleared or the boundary is freed
                    // but the hgrid is not yet updated.
                    if *pid >= boundary.positions.len() {
                        return None;
                    }
                    // --- END FIX ---
                    
                    let pt = boundary.positions[*pid];
                    if aabb.distance_to_local_point(pt.into(), true) < self.particle_radius {
                        Some(ParticleId::BoundaryParticle(handle, *pid))
                    } else {
                        None
                    }
                }
            })
    }

    /// The set of particles potentially intersecting the given shape.
    #[cfg(feature = "parry")]
    pub fn particles_intersecting_shape<'a, S: ?Sized>(
        &'a self,
        pos: &'a Isometry<Real>,
        shape: &'a S,
    ) -> impl Iterator<Item = ParticleId> + 'a
    where
        S: Shape,
    {
        let pos = (*pos).into();
        let aabb = shape.compute_aabb(&pos);
        let mins = aabb.mins.into();
        let maxs = aabb.maxs.into();
        self.hgrid
            .cells_intersecting_aabb(&mins, &maxs)
            .flat_map(|e| e.1)
            .filter_map(move |entry| match entry {
                HGridEntry::FluidParticle(fid, pid) => {
                    let (fluid, handle) = self.fluids.get_from_contiguous_index(*fid)?;

                    // Defensive bounds check for fluids
                    if *pid >= fluid.positions.len() {
                        return None;
                    }

                    let pt = fluid.positions[*pid];

                    if shape.distance_to_point(&pos, pt.into(), true) <= self.particle_radius {
                        Some(ParticleId::FluidParticle(handle, *pid))
                    } else {
                        None
                    }
                }
                HGridEntry::BoundaryParticle(bid, pid) => {
                    let (boundary, handle) = self.boundaries.get_from_contiguous_index(*bid)?;

                    // --- DEFENSIVE FIX ---
                    if *pid >= boundary.positions.len() {
                        return None;
                    }
                    // --- END FIX ---

                    let pt = boundary.positions[*pid];
                    if shape.distance_to_point(&pos, pt.into(), true) <= self.particle_radius {
                        Some(ParticleId::BoundaryParticle(handle, *pid))
                    } else {
                        None
                    }
                }
            })
    }
}

#[test]
fn world_is_send_and_sync() {
    fn check<T: Send + Sync>() {}
    check::<LiquidWorld>();
}
