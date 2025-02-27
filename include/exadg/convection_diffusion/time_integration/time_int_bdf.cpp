/*  ______________________________________________________________________
 *
 *  ExaDG - High-Order Discontinuous Galerkin for the Exa-Scale
 *
 *  Copyright (C) 2021 by the ExaDG authors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *  ______________________________________________________________________
 */

#include <exadg/convection_diffusion/postprocessor/postprocessor_base.h>
#include <exadg/convection_diffusion/spatial_discretization/operator.h>
#include <exadg/convection_diffusion/time_integration/time_int_bdf.h>
#include <exadg/convection_diffusion/user_interface/parameters.h>
#include <exadg/time_integration/push_back_vectors.h>
#include <exadg/time_integration/restart.h>
#include <exadg/time_integration/time_step_calculation.h>
#include <exadg/utilities/print_solver_results.h>

namespace ExaDG
{
namespace ConvDiff
{
template<int dim, typename Number>
TimeIntBDF<dim, Number>::TimeIntBDF(
  std::shared_ptr<Operator<dim, Number>>          operator_in,
  std::shared_ptr<HelpersALE<dim, Number> const>  helpers_ale_in,
  std::shared_ptr<PostProcessorInterface<Number>> postprocessor_in,
  Parameters const &                              param_in,
  MPI_Comm const &                                mpi_comm_in,
  bool const                                      is_test_in)
  : TimeIntBDFBase(param_in.start_time,
                   param_in.end_time,
                   param_in.max_number_of_time_steps,
                   param_in.order_time_integrator,
                   param_in.start_with_low_order,
                   param_in.adaptive_time_stepping,
                   param_in.restart_data,
                   mpi_comm_in,
                   is_test_in),
    pde_operator(operator_in),
    param(param_in),
    refine_steps_time(param_in.n_refine_time),
    cfl(param.cfl / std::pow(2.0, refine_steps_time)),
    solution(param_in.order_time_integrator),
    vec_convective_term(param_in.order_time_integrator),
    iterations({0, 0}),
    postprocessor(postprocessor_in),
    helpers_ale(helpers_ale_in),
    vec_grid_coordinates(param_in.order_time_integrator)
{
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::setup_derived()
{
  // In the case of an arbitrary Lagrangian-Eulerian formulation:
  if(param.ale_formulation and param.restarted_simulation == false)
  {
    // compute the grid coordinates at start time (and at previous times in case of
    // start_with_low_order == false)

    helpers_ale->move_grid(this->get_time());
    helpers_ale->fill_grid_coordinates_vector(vec_grid_coordinates[0],
                                              pde_operator->get_dof_handler_velocity());

    if(this->start_with_low_order == false)
    {
      // compute grid coordinates at previous times (start with 1!)
      for(unsigned int i = 1; i < this->order; ++i)
      {
        helpers_ale->move_grid(this->get_previous_time(i));
        helpers_ale->fill_grid_coordinates_vector(vec_grid_coordinates[i],
                                                  pde_operator->get_dof_handler_velocity());
      }
    }
  }

  // Initialize vec_convective_term: Note that this function has to be called
  // after the solution has been initialized because the solution is evaluated in this function.
  if(param.convective_problem() and
     param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    // vec_convective_term does not have to be initialized in ALE case (the convective
    // term is recomputed in each time step for all previous times on the new mesh).
    // vec_convective_term does not have to be initialized in case of a restart, where
    // the vectors are read from memory.
    if(param.ale_formulation == false and param.restarted_simulation == false)
    {
      initialize_vec_convective_term();
    }
  }
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::allocate_vectors()
{
  for(unsigned int i = 0; i < solution.size(); ++i)
    pde_operator->initialize_dof_vector(solution[i]);

  pde_operator->initialize_dof_vector(solution_np);

  pde_operator->initialize_dof_vector(rhs_vector);

  if(param.convective_problem())
  {
    if(param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
    {
      for(unsigned int i = 0; i < vec_convective_term.size(); ++i)
        pde_operator->initialize_dof_vector(vec_convective_term[i]);

      if(param.ale_formulation == false)
        pde_operator->initialize_dof_vector(convective_term_np);
    }
  }

  if(param.ale_formulation == true)
  {
    pde_operator->initialize_dof_vector_velocity(grid_velocity);

    pde_operator->initialize_dof_vector_velocity(grid_coordinates_np);

    for(unsigned int i = 0; i < vec_grid_coordinates.size(); ++i)
      pde_operator->initialize_dof_vector_velocity(vec_grid_coordinates[i]);
  }
}

template<int dim, typename Number>
std::shared_ptr<std::vector<dealii::LinearAlgebra::distributed::Vector<Number> *>>
TimeIntBDF<dim, Number>::get_vectors()
{
  std::shared_ptr<std::vector<VectorType *>> vectors =
    std::make_shared<std::vector<VectorType *>>();

  for(unsigned int i = 0; i < this->order; i++)
  {
    vectors->emplace_back(&solution[i]);
  }

  vectors->emplace_back(&solution_np);

  vectors->emplace_back(&rhs_vector);

  if(param.convective_problem() and
     param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    for(unsigned int i = 0; i < this->order; i++)
    {
      vectors->emplace_back(&vec_convective_term[i]);
    }

    if(param.ale_formulation == false)
    {
      vectors->emplace_back(&convective_term_np);
    }
  }

  if(this->param.ale_formulation)
  {
    vectors->emplace_back(&grid_velocity);

    vectors->emplace_back(&grid_coordinates_np);

    for(unsigned int i = 0; i < vec_grid_coordinates.size(); i++)
    {
      vectors->emplace_back(&vec_grid_coordinates[i]);
    }
  }

  return vectors;
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::prepare_coarsening_and_refinement()
{
  std::shared_ptr<std::vector<VectorType *>> vectors = get_vectors();
  pde_operator->prepare_coarsening_and_refinement(*vectors);
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::interpolate_after_coarsening_and_refinement()
{
  this->allocate_vectors();

  std::shared_ptr<std::vector<VectorType *>> vectors = get_vectors();
  pde_operator->interpolate_after_coarsening_and_refinement(*vectors);
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::initialize_current_solution()
{
  if(this->param.ale_formulation)
    helpers_ale->move_grid(this->get_time());

  pde_operator->prescribe_initial_conditions(solution[0], this->get_time());
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::initialize_former_multistep_dof_vectors()
{
  // Start with i=1 since we only want to initialize the solution at former instants of time.
  for(unsigned int i = 1; i < solution.size(); ++i)
  {
    if(this->param.ale_formulation)
      helpers_ale->move_grid(this->get_previous_time(i));

    pde_operator->prescribe_initial_conditions(solution[i], this->get_previous_time(i));
  }
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::initialize_vec_convective_term()
{
  if(this->param.get_type_velocity_field() != TypeVelocityField::DoFVector)
  {
    pde_operator->evaluate_convective_term(vec_convective_term[0], solution[0], this->get_time());

    if(this->param.start_with_low_order == false)
    {
      for(unsigned int i = 1; i < vec_convective_term.size(); ++i)
      {
        pde_operator->evaluate_convective_term(vec_convective_term[i],
                                               solution[i],
                                               this->get_previous_time(i));
      }
    }
  }
}

template<int dim, typename Number>
double
TimeIntBDF<dim, Number>::calculate_time_step_size()
{
  double time_step = 1.0;

  if(param.calculation_of_time_step_size == TimeStepCalculation::UserSpecified)
  {
    time_step = calculate_const_time_step(param.time_step_size, refine_steps_time);

    this->pcout << std::endl
                << "Calculation of time step size (user-specified):" << std::endl
                << std::endl;
    print_parameter(this->pcout, "time step size", time_step);
  }
  else if(param.calculation_of_time_step_size == TimeStepCalculation::CFL)
  {
    AssertThrow(param.convective_problem(),
                dealii::ExcMessage("Specified type of time step calculation does not make sense!"));

    double time_step_global = pde_operator->calculate_time_step_cfl_global(this->get_time());
    time_step_global *= cfl;

    this->pcout << std::endl
                << "Calculation of time step size according to CFL condition:" << std::endl
                << std::endl;
    print_parameter(this->pcout, "CFL", cfl);
    print_parameter(this->pcout, "Time step size (CFL global)", time_step_global);

    if(this->adaptive_time_stepping == true)
    {
      double time_step_adap = std::numeric_limits<double>::max();

      // Note that in the ALE case there is no possibility to know the grid velocity at this point
      // and to use it for the calculation of the time step size.

      if(param.analytical_velocity_field)
      {
        time_step_adap =
          pde_operator->calculate_time_step_cfl_analytical_velocity(this->get_time());
        time_step_adap *= cfl;
      }
      else
      {
        // do nothing (the velocity field is not known at this point)
      }

      // use adaptive time step size only if it is smaller, otherwise use global time step size
      time_step = std::min(time_step_adap, time_step_global);

      // make sure that the maximum allowable time step size is not exceeded
      time_step = std::min(time_step, param.time_step_size_max);

      print_parameter(this->pcout, "Time step size (CFL adaptive)", time_step);
    }
    else // constant time step size
    {
      time_step =
        adjust_time_step_to_hit_end_time(param.start_time, param.end_time, time_step_global);

      this->pcout << std::endl
                  << "Adjust time step size to hit end time:" << std::endl
                  << std::endl;
      print_parameter(this->pcout, "Time step size", time_step);
    }
  }
  else if(param.calculation_of_time_step_size == TimeStepCalculation::MaxEfficiency)
  {
    time_step          = pde_operator->calculate_time_step_max_efficiency(this->order);
    double const c_eff = param.c_eff / std::pow(2., refine_steps_time);
    time_step *= c_eff;

    time_step = adjust_time_step_to_hit_end_time(param.start_time, param.end_time, time_step);

    this->pcout << std::endl
                << "Calculation of time step size (max efficiency):" << std::endl
                << std::endl;
    print_parameter(this->pcout, "C_eff", c_eff);
    print_parameter(this->pcout, "Time step size", time_step);
  }
  else
  {
    AssertThrow(false,
                dealii::ExcMessage("Specified type of time step calculation is not implemented."));
  }

  return time_step;
}

template<int dim, typename Number>
double
TimeIntBDF<dim, Number>::recalculate_time_step_size() const
{
  AssertThrow(param.calculation_of_time_step_size == TimeStepCalculation::CFL,
              dealii::ExcMessage(
                "Adaptive time step is not implemented for this type of time step calculation."));

  double new_time_step_size = std::numeric_limits<double>::max();
  if(param.analytical_velocity_field)
  {
    new_time_step_size =
      pde_operator->calculate_time_step_cfl_analytical_velocity(this->get_time());
    new_time_step_size *= cfl;
  }
  else // numerical velocity field
  {
    AssertThrow(velocities[0] != nullptr,
                dealii::ExcMessage("Pointer velocities[0] is not initialized."));

    VectorType u_relative = *velocities[0];
    if(param.ale_formulation == true)
      u_relative -= grid_velocity;

    new_time_step_size = pde_operator->calculate_time_step_cfl_numerical_velocity(u_relative);
    new_time_step_size *= cfl;
  }

  // make sure that time step size does not exceed maximum allowable time step size
  new_time_step_size = std::min(new_time_step_size, param.time_step_size_max);

  bool use_limiter = true;
  if(use_limiter)
  {
    double last_time_step_size = this->get_time_step_size();
    double factor              = param.adaptive_time_stepping_limiting_factor;
    limit_time_step_change(new_time_step_size, last_time_step_size, factor);
  }

  return new_time_step_size;
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::prepare_vectors_for_next_timestep()
{
  push_back(solution);

  solution[0].swap(solution_np);

  if(param.convective_problem() and
     param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    if(param.ale_formulation == false)
    {
      push_back(vec_convective_term);
      vec_convective_term[0].swap(convective_term_np);
    }
  }

  if(param.ale_formulation)
  {
    push_back(vec_grid_coordinates);
    vec_grid_coordinates[0].swap(grid_coordinates_np);
  }
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::ale_update()
{
  // and compute grid coordinates at the end of the current time step t_{n+1}
  helpers_ale->fill_grid_coordinates_vector(grid_coordinates_np,
                                            pde_operator->get_dof_handler_velocity());

  // and update grid velocity using BDF time derivative
  compute_bdf_time_derivative(grid_velocity,
                              grid_coordinates_np,
                              vec_grid_coordinates,
                              this->bdf,
                              this->get_time_step_size());
}

template<int dim, typename Number>
bool
TimeIntBDF<dim, Number>::print_solver_info() const
{
  return param.solver_info_data.write(this->global_timer.wall_time(),
                                      this->time,
                                      this->time_step_number);
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::read_restart_vectors(BoostInputArchiveType & ia)
{
  for(unsigned int i = 0; i < this->order; i++)
  {
    read_write_distributed_vector(solution[i], ia);
  }

  if(param.convective_problem() and
     param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    if(this->param.ale_formulation == false)
    {
      for(unsigned int i = 0; i < this->order; i++)
      {
        read_write_distributed_vector(vec_convective_term[i], ia);
      }
    }
  }

  if(this->param.ale_formulation)
  {
    for(unsigned int i = 0; i < vec_grid_coordinates.size(); i++)
    {
      read_write_distributed_vector(vec_grid_coordinates[i], ia);
    }
  }
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::write_restart_vectors(BoostOutputArchiveType & oa) const
{
  for(unsigned int i = 0; i < this->order; i++)
  {
    read_write_distributed_vector(solution[i], oa);
  }

  if(param.convective_problem() and
     param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    if(this->param.ale_formulation == false)
    {
      for(unsigned int i = 0; i < this->order; i++)
      {
        read_write_distributed_vector(vec_convective_term[i], oa);
      }
    }
  }

  if(this->param.ale_formulation)
  {
    for(unsigned int i = 0; i < vec_grid_coordinates.size(); i++)
    {
      read_write_distributed_vector(vec_grid_coordinates[i], oa);
    }
  }
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::do_timestep_solve()
{
  dealii::Timer timer;
  timer.restart();

  // transport velocity
  VectorType velocity_np;

  if(param.convective_problem())
  {
    if(param.get_type_velocity_field() == TypeVelocityField::DoFVector)
    {
      pde_operator->initialize_dof_vector_velocity(velocity_np);

      if(param.analytical_velocity_field)
      {
        pde_operator->project_velocity(velocity_np, this->get_next_time());
      }
      else
      {
        AssertThrow(std::abs(times[0] - this->get_next_time()) < 1.e-12 * param.end_time,
                    dealii::ExcMessage("Invalid assumption."));
        AssertThrow(velocities[0] != nullptr,
                    dealii::ExcMessage("Pointer velocities[0] is not correctly initialized."));

        velocity_np = *velocities[0];
      }

      if(param.ale_formulation)
      {
        velocity_np -= grid_velocity;
      }
    }
    else
    {
      AssertThrow(param.ale_formulation == false, dealii::ExcMessage("not implemented."));
    }
  }

  // calculate rhs (rhs-vector f and inhomogeneous boundary face integrals)
  pde_operator->rhs(rhs_vector, this->get_next_time(), &velocity_np);

  // if the convective term is involved in the equations:
  // add the convective term to the right-hand side of the equations
  // if this term is treated explicitly (additive decomposition)
  if(param.convective_problem() and
     param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    // recompute convective term on new mesh for all previous time instants in case of
    // ALE formulation
    if(param.ale_formulation == true)
    {
      for(unsigned int i = 0; i < vec_convective_term.size(); ++i)
      {
        pde_operator->evaluate_convective_term(vec_convective_term[i],
                                               solution[i],
                                               this->get_previous_time(i),
                                               &velocity_np);
      }
    }

    for(unsigned int i = 0; i < vec_convective_term.size(); ++i)
      rhs_vector.add(-this->extra.get_beta(i), vec_convective_term[i]);
  }

  VectorType sum_alphai_ui(solution[0]);
  sum_alphai_ui.equ(this->bdf.get_alpha(0) / this->get_time_step_size(), solution[0]);
  for(unsigned int i = 1; i < solution.size(); ++i)
    sum_alphai_ui.add(this->bdf.get_alpha(i) / this->get_time_step_size(), solution[i]);

  // apply mass operator to sum_alphai_ui and add to rhs_vector
  pde_operator->apply_mass_operator_add(rhs_vector, sum_alphai_ui);

  // extrapolate old solution to obtain a good initial guess for the solver
  solution_np.equ(this->extra.get_beta(0), solution[0]);
  for(unsigned int i = 1; i < solution.size(); ++i)
    solution_np.add(this->extra.get_beta(i), solution[i]);

  // solve the linear system of equations
  bool const update_preconditioner =
    this->param.update_preconditioner and
    (this->time_step_number % this->param.update_preconditioner_every_time_steps == 0);

  unsigned int const N_iter =
    pde_operator->solve(solution_np,
                        rhs_vector,
                        update_preconditioner,
                        this->bdf.get_gamma0() / this->get_time_step_size(),
                        this->get_next_time(),
                        &velocity_np);

  iterations.first += 1;
  iterations.second += N_iter;

  // evaluate convective term at end time t_{n+1} at which we know the boundary condition
  // g_u(t_{n+1})
  if(param.convective_problem() and
     param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    if(param.ale_formulation == false)
    {
      if(param.get_type_velocity_field() == TypeVelocityField::DoFVector)
      {
        pde_operator->evaluate_convective_term(convective_term_np,
                                               solution_np,
                                               this->get_next_time(),
                                               &velocity_np);
      }
      else
      {
        pde_operator->evaluate_convective_term(convective_term_np,
                                               solution_np,
                                               this->get_next_time());
      }
    }
  }

  if(print_solver_info() and not(this->is_test))
  {
    this->pcout << std::endl << "Solve scalar convection-diffusion equation:";
    print_solver_info_linear(this->pcout, N_iter, timer.wall_time());
  }

  this->timer_tree->insert({"Timeloop", "Solve"}, timer.wall_time());
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::postprocessing() const
{
  dealii::Timer timer;
  timer.restart();

  // To allow a computation of errors at start_time (= if time step number is 1 and if the
  // simulation is not a restarted one), the mesh has to be at the correct position
  if(this->param.ale_formulation and this->get_time_step_number() == 1 and
     not this->param.restarted_simulation)
  {
    helpers_ale->move_grid(this->get_time());
    helpers_ale->update_pde_operator_after_grid_motion();
  }

  postprocessor->do_postprocessing(solution[0], this->get_time(), this->get_time_step_number());

  this->timer_tree->insert({"Timeloop", "Postprocessing"}, timer.wall_time());
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::print_iterations() const
{
  std::vector<std::string> names = {"Linear system"};

  std::vector<double> iterations_avg;
  iterations_avg.resize(1);
  iterations_avg[0] = (double)iterations.second / std::max(1., (double)iterations.first);

  print_list_of_iterations(this->pcout, names, iterations_avg);
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::set_velocities_and_times(
  std::vector<VectorType const *> const & velocities_in,
  std::vector<double> const &             times_in)
{
  velocities = velocities_in;
  times      = times_in;
}

template<int dim, typename Number>
dealii::LinearAlgebra::distributed::Vector<Number> const &
TimeIntBDF<dim, Number>::get_solution_np() const
{
  return (this->solution_np);
}

template<int dim, typename Number>
void
TimeIntBDF<dim, Number>::extrapolate_solution(VectorType & vector)
{
  // make sure that the time integrator constants are up-to-date
  this->update_time_integrator_constants();

  vector.equ(this->extra.get_beta(0), this->solution[0]);
  for(unsigned int i = 1; i < solution.size(); ++i)
    vector.add(this->extra.get_beta(i), this->solution[i]);
}

// instantiations

template class TimeIntBDF<2, float>;
template class TimeIntBDF<2, double>;

template class TimeIntBDF<3, float>;
template class TimeIntBDF<3, double>;

} // namespace ConvDiff
} // namespace ExaDG
