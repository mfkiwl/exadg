/*
 * time_int_bdf_pressure_correction.h
 *
 *  Created on: Oct 26, 2016
 *      Author: fehn
 */

#ifndef INCLUDE_INCOMPRESSIBLE_NAVIER_STOKES_TIME_INTEGRATION_TIME_INT_BDF_PRESSURE_CORRECTION_H_
#define INCLUDE_INCOMPRESSIBLE_NAVIER_STOKES_TIME_INTEGRATION_TIME_INT_BDF_PRESSURE_CORRECTION_H_

#include "../../incompressible_navier_stokes/time_integration/time_int_bdf_navier_stokes.h"
#include "../../time_integration/push_back_vectors.h"

namespace IncNS
{
template<int dim, typename Number, typename NavierStokesOperation>
class TimeIntBDFPressureCorrection : public TimeIntBDF<dim, Number, NavierStokesOperation>
{
public:
  typedef TimeIntBDF<dim, Number, NavierStokesOperation> Base;

  typedef typename Base::VectorType VectorType;

  TimeIntBDFPressureCorrection(std::shared_ptr<NavierStokesOperation> navier_stokes_operation_in,
                               InputParameters<dim> const &           param_in,
                               unsigned int const                     n_refine_time_in,
                               bool const                             use_adaptive_time_stepping);

  virtual ~TimeIntBDFPressureCorrection()
  {
  }

  virtual void
  analyze_computing_times() const;

  void
  postprocessing_stability_analysis();

private:
  virtual void
  update_time_integrator_constants();

  virtual void
  allocate_vectors();

  virtual void
  initialize_current_solution();

  virtual void
  initialize_former_solutions();

  virtual void
  setup_derived();

  void
  initialize_vec_convective_term();

  void
  initialize_vec_pressure_gradient_term();

  virtual void
  solve_timestep();

  virtual void
  solve_steady_problem();

  double
  evaluate_residual();

  void
  momentum_step();

  void
  rhs_momentum();

  void
  pressure_step();

  void
  projection_step();

  void
  rhs_projection();

  void
  pressure_update();

  void
  calculate_chi(double & chi) const;

  void
  rhs_pressure();

  virtual void
  prepare_vectors_for_next_timestep();

  virtual void
  postprocessing() const;

  virtual void
  postprocessing_steady_problem() const;

  virtual LinearAlgebra::distributed::Vector<Number> const &
  get_velocity() const;

  virtual LinearAlgebra::distributed::Vector<Number> const &
  get_velocity(unsigned int i /* t_{n-i} */) const;

  virtual LinearAlgebra::distributed::Vector<Number> const &
  get_pressure(unsigned int i /* t_{n-i} */) const;

  virtual void
  set_velocity(VectorType const & velocity, unsigned int const i /* t_{n-i} */);

  virtual void
  set_pressure(VectorType const & pressure, unsigned int const i /* t_{n-i} */);

  std::shared_ptr<NavierStokesOperation> navier_stokes_operation;

  VectorType              velocity_np;
  std::vector<VectorType> velocity;

  VectorType              pressure_np;
  std::vector<VectorType> pressure;

  VectorType pressure_increment;

  std::vector<VectorType> vec_convective_term;

  // rhs vector momentum step
  VectorType rhs_vec_momentum;

  // rhs vector pressur step
  VectorType rhs_vec_pressure;
  VectorType rhs_vec_pressure_temp;

  // rhs vector projection step
  VectorType rhs_vec_projection;
  VectorType rhs_vec_projection_temp;

  // incremental formulation of pressure-correction scheme
  unsigned int order_pressure_extrapolation;

  // time integrator constants: extrapolation scheme
  ExtrapolationConstants extra_pressure_gradient;

  std::vector<VectorType> vec_pressure_gradient_term;

  std::vector<Number>       computing_times;
  std::vector<unsigned int> iterations;

  unsigned int N_iter_nonlinear_momentum;

  // temporary vectors needed for pseudo-timestepping algorithm
  VectorType velocity_tmp;
  VectorType pressure_tmp;
};

template<int dim, typename Number, typename NavierStokesOperation>
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::TimeIntBDFPressureCorrection(
  std::shared_ptr<NavierStokesOperation> navier_stokes_operation_in,
  InputParameters<dim> const &           param_in,
  unsigned int const                     n_refine_time_in,
  bool const                             use_adaptive_time_stepping)
  : TimeIntBDF<dim, Number, NavierStokesOperation>(navier_stokes_operation_in,
                                                   param_in,
                                                   n_refine_time_in,
                                                   use_adaptive_time_stepping),
    navier_stokes_operation(navier_stokes_operation_in),
    velocity(param_in.order_time_integrator),
    pressure(param_in.order_time_integrator),
    vec_convective_term(param_in.order_time_integrator),
    order_pressure_extrapolation(param_in.order_pressure_extrapolation),
    extra_pressure_gradient(param_in.order_pressure_extrapolation, param_in.start_with_low_order),
    vec_pressure_gradient_term(param_in.order_pressure_extrapolation),
    computing_times(3),
    iterations(3),
    N_iter_nonlinear_momentum(0)
{
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::update_time_integrator_constants()
{
  // call function of base class to update the standard time integrator constants
  TimeIntBDF<dim, Number, NavierStokesOperation>::update_time_integrator_constants();

  // update time integrator constants for extrapolation scheme of pressure gradient term

  // if start_with_low_order == true (no analytical solution available)
  // the pressure is unknown at t = t_0:
  // -> use no extrapolation (order=0, non-incremental) in first time step (the pressure solution is
  // calculated in the second sub step)
  // -> use first order extrapolation in second time step, second order extrapolation in third time
  // step, etc.
  if(this->adaptive_time_stepping == false)
  {
    extra_pressure_gradient.update(this->get_time_step_number() - 1);
  }
  else // adaptive time stepping
  {
    extra_pressure_gradient.update(this->get_time_step_number() - 1, this->get_time_step_vector());
  }

  // use this function to check the correctness of the time integrator constants
  //  std::cout << "Coefficients extrapolation scheme pressure: Time step = " <<
  //  this->get_time_step_number() << std::endl; extra_pressure_gradient.print();
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::setup_derived()
{
  if(this->param.equation_type == EquationType::NavierStokes &&
     this->start_with_low_order == false &&
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
    initialize_vec_convective_term();

  if(extra_pressure_gradient.get_order() > 0)
    initialize_vec_pressure_gradient_term();
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::allocate_vectors()
{
  // velocity
  for(unsigned int i = 0; i < velocity.size(); ++i)
    navier_stokes_operation->initialize_vector_velocity(velocity[i]);
  navier_stokes_operation->initialize_vector_velocity(velocity_np);

  // pressure
  for(unsigned int i = 0; i < pressure.size(); ++i)
    navier_stokes_operation->initialize_vector_pressure(pressure[i]);
  navier_stokes_operation->initialize_vector_pressure(pressure_np);
  navier_stokes_operation->initialize_vector_pressure(pressure_increment);

  // vec_convective_term
  if(this->param.equation_type == EquationType::NavierStokes &&
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    for(unsigned int i = 0; i < vec_convective_term.size(); ++i)
      navier_stokes_operation->initialize_vector_velocity(vec_convective_term[i]);
  }

  // vec_pressure_gradient_term
  for(unsigned int i = 0; i < vec_pressure_gradient_term.size(); ++i)
    navier_stokes_operation->initialize_vector_velocity(vec_pressure_gradient_term[i]);

  // Sum_i (alpha_i/dt * u_i)
  navier_stokes_operation->initialize_vector_velocity(this->sum_alphai_ui);

  // rhs vector momentum
  navier_stokes_operation->initialize_vector_velocity(rhs_vec_momentum);

  // rhs vector pressure
  navier_stokes_operation->initialize_vector_pressure(rhs_vec_pressure);
  navier_stokes_operation->initialize_vector_pressure(rhs_vec_pressure_temp);

  // rhs vector projection
  navier_stokes_operation->initialize_vector_velocity(rhs_vec_projection);
  navier_stokes_operation->initialize_vector_velocity(rhs_vec_projection_temp);
}


template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::initialize_current_solution()
{
  navier_stokes_operation->prescribe_initial_conditions(velocity[0], pressure[0], this->get_time());
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::initialize_former_solutions()
{
  // note that the loop begins with i=1! (we could also start with i=0 but this is not necessary)
  for(unsigned int i = 1; i < velocity.size(); ++i)
  {
    navier_stokes_operation->prescribe_initial_conditions(velocity[i],
                                                          pressure[i],
                                                          this->get_previous_time(i));
  }
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::initialize_vec_convective_term()
{
  // note that the loop begins with i=1! (we could also start with i=0 but this is not necessary)
  for(unsigned int i = 1; i < vec_convective_term.size(); ++i)
  {
    navier_stokes_operation->evaluate_convective_term(vec_convective_term[i],
                                                      velocity[i],
                                                      this->get_previous_time(i));
  }
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::
  initialize_vec_pressure_gradient_term()
{
  // note that the loop begins with i=1! (we could also start with i=0 but this is not necessary)
  for(unsigned int i = 1; i < vec_pressure_gradient_term.size(); ++i)
  {
    navier_stokes_operation->evaluate_pressure_gradient_term(vec_pressure_gradient_term[i],
                                                             pressure[i],
                                                             this->get_previous_time(i));
  }
}

template<int dim, typename Number, typename NavierStokesOperation>
LinearAlgebra::distributed::Vector<Number> const &
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::get_velocity() const
{
  return velocity[0];
}

template<int dim, typename Number, typename NavierStokesOperation>
LinearAlgebra::distributed::Vector<Number> const &
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::get_velocity(unsigned int i) const
{
  return velocity[i];
}

template<int dim, typename Number, typename NavierStokesOperation>
LinearAlgebra::distributed::Vector<Number> const &
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::get_pressure(unsigned int i) const
{
  return pressure[i];
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::set_velocity(
  VectorType const & velocity_in,
  unsigned int const i)
{
  velocity[i] = velocity_in;
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::set_pressure(
  VectorType const & pressure_in,
  unsigned int const i)
{
  pressure[i] = pressure_in;
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::postprocessing() const
{
  navier_stokes_operation->do_postprocessing(velocity[0],
                                             pressure[0],
                                             this->get_time(),
                                             this->get_time_step_number());
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::postprocessing_steady_problem()
  const
{
  navier_stokes_operation->do_postprocessing_steady_problem(velocity[0], pressure[0]);
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::
  postprocessing_stability_analysis()
{
  AssertThrow(this->order == 1,
              ExcMessage("Order of BDF scheme has to be 1 for this stability analysis."));

  AssertThrow(velocity[0].l2_norm() < 1.e-15 && pressure[0].l2_norm() < 1.e-15,
              ExcMessage("Solution vector has to be zero for this stability analysis."));

  AssertThrow(Utilities::MPI::n_mpi_processes(MPI_COMM_WORLD) == 1,
              ExcMessage("Number of MPI processes has to be 1."));

  std::cout << std::endl << "Analysis of eigenvalue spectrum:" << std::endl;

  const unsigned int size = velocity[0].local_size();

  LAPACKFullMatrix<Number> propagation_matrix(size, size);

  // loop over all columns of propagation matrix
  for(unsigned int j = 0; j < size; ++j)
  {
    // set j-th element to 1
    velocity[0].local_element(j) = 1.0;

    // solve time step
    solve_timestep();

    // dst-vector velocity_np is j-th column of propagation matrix
    for(unsigned int i = 0; i < size; ++i)
    {
      propagation_matrix(i, j) = velocity_np.local_element(i);
    }

    // reset j-th element to 0
    velocity[0].local_element(j) = 0.0;
  }

  // compute eigenvalues
  propagation_matrix.compute_eigenvalues();

  double norm_max = 0.0;

  std::cout << "List of all eigenvalues:" << std::endl;

  for(unsigned int i = 0; i < size; ++i)
  {
    double norm = std::abs(propagation_matrix.eigenvalue(i));
    if(norm > norm_max)
      norm_max = norm;

    // print eigenvalues
    std::cout << std::scientific << std::setprecision(5) << propagation_matrix.eigenvalue(i)
              << std::endl;
  }

  std::cout << std::endl << std::endl << "Maximum eigenvalue = " << norm_max << std::endl;
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::solve_timestep()
{
  // perform the substeps of the pressure-correction scheme
  momentum_step();

  pressure_step();

  projection_step();
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::momentum_step()
{
  Timer timer;
  timer.restart();

  /*
   *  Extrapolate old solution to get a good initial estimate for the solver.
   */
  velocity_np = 0.0;
  for(unsigned int i = 0; i < velocity.size(); ++i)
  {
    velocity_np.add(this->extra.get_beta(i), velocity[i]);
  }

  /*
   *  if a turbulence model is used:
   *  update turbulence model before calculating rhs_momentum
   */
  if(this->param.use_turbulence_model == true)
  {
    Timer timer_turbulence;
    timer_turbulence.restart();

    navier_stokes_operation->update_turbulence_model(velocity_np);

    if(this->get_time_step_number() % this->param.output_solver_info_every_timesteps == 0)
    {
      this->pcout << std::endl
                  << "Update of turbulent viscosity:   Wall time [s]: " << std::scientific
                  << timer_turbulence.wall_time() << std::endl;
    }
  }


  /*
   *  Calculate the right-hand side of the linear system of equations
   *  (in case of an explicit formulation of the convective term or Stokes equations)
   *  or the vector that is constant when solving the nonlinear momentum equation
   *  (where constant means that the vector does not change from one Newton iteration
   *  to the next, i.e., it does not depend on the current solution of the nonlinear solver)
   */
  rhs_momentum();

  /*
   *  Solve the linear or nonlinear problem.
   */
  if(this->param.equation_type == EquationType::Stokes ||
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit ||
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::ExplicitOIF)
  {
    // solve linear system of equations
    unsigned int linear_iterations_momentum;
    navier_stokes_operation->solve_linear_momentum_equation(
      velocity_np,
      rhs_vec_momentum,
      this->get_scaling_factor_time_derivative_term(),
      linear_iterations_momentum);

    // write output explicit case
    if(this->get_time_step_number() % this->param.output_solver_info_every_timesteps == 0)
    {
      this->pcout << std::endl
                  << "Solve linear momentum equation for intermediate velocity:" << std::endl
                  << "  Iterations:        " << std::setw(6) << std::right
                  << linear_iterations_momentum << "\t Wall time [s]: " << std::scientific
                  << timer.wall_time() << std::endl;
    }

    iterations[0] += linear_iterations_momentum;
  }
  else // treatment of convective term == Implicit
  {
    AssertThrow(
      this->param.equation_type == EquationType::NavierStokes &&
        this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Implicit,
      ExcMessage(
        "There is a logical error. Probably, the specified combination of input parameters is not implemented."));

    unsigned int linear_iterations_momentum;
    unsigned int nonlinear_iterations_momentum;
    navier_stokes_operation->solve_nonlinear_momentum_equation(
      velocity_np,
      rhs_vec_momentum,
      this->get_next_time(),
      this->get_scaling_factor_time_derivative_term(),
      nonlinear_iterations_momentum,
      linear_iterations_momentum);

    // write output implicit case
    if(this->get_time_step_number() % this->param.output_solver_info_every_timesteps == 0)
    {
      this->pcout << std::endl
                  << "Solve nonlinear momentum equation for intermediate velocity:" << std::endl
                  << "  Newton iterations: " << std::setw(6) << std::right
                  << nonlinear_iterations_momentum << "\t Wall time [s]: " << std::scientific
                  << timer.wall_time() << std::endl
                  << "  Linear iterations: " << std::setw(6) << std::right << std::fixed
                  << std::setprecision(2)
                  << (nonlinear_iterations_momentum > 0 ?
                        (double)linear_iterations_momentum / (double)nonlinear_iterations_momentum :
                        linear_iterations_momentum)
                  << " (avg)" << std::endl
                  << "  Linear iterations: " << std::setw(6) << std::right << std::fixed
                  << std::setprecision(2) << linear_iterations_momentum << " (tot)" << std::endl;
    }

    iterations[0] += linear_iterations_momentum;
    N_iter_nonlinear_momentum += nonlinear_iterations_momentum;
  }

  computing_times[0] += timer.wall_time();
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::rhs_momentum()
{
  rhs_vec_momentum = 0;

  /*
   *  Add extrapolation of pressure gradient term to the rhs in case of incremental formulation
   */
  if(extra_pressure_gradient.get_order() > 0)
  {
    navier_stokes_operation->evaluate_pressure_gradient_term(vec_pressure_gradient_term[0],
                                                             pressure[0],
                                                             this->get_time());

    for(unsigned int i = 0; i < extra_pressure_gradient.get_order(); ++i)
      rhs_vec_momentum.add(-extra_pressure_gradient.get_beta(i), vec_pressure_gradient_term[i]);
  }

  /*
   *  Body force term
   */
  if(this->param.right_hand_side == true)
  {
    navier_stokes_operation->evaluate_add_body_force_term(rhs_vec_momentum, this->get_next_time());
  }

  /*
   *  Convective term formulated explicitly (additive decomposition):
   *  Evaluate convective term and add extrapolation of convective term to the rhs
   */
  if(this->param.equation_type == EquationType::NavierStokes &&
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    navier_stokes_operation->evaluate_convective_term(vec_convective_term[0],
                                                      velocity[0],
                                                      this->get_time());

    for(unsigned int i = 0; i < vec_convective_term.size(); ++i)
      rhs_vec_momentum.add(-this->extra.get_beta(i), vec_convective_term[i]);
  }

  /*
   *  calculate sum (alpha_i/dt * u_i): This term is relevant for both the explicit
   *  and the implicit formulation of the convective term
   */
  // calculate sum (alpha_i/dt * u_tilde_i) in case of explicit treatment of convective term
  // and operator-integration-factor (OIF) splitting
  if(this->param.equation_type == EquationType::NavierStokes &&
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::ExplicitOIF)
  {
    this->calculate_sum_alphai_ui_oif_substepping(this->cfl, this->cfl_oif);
  }
  // calculate sum (alpha_i/dt * u_i) for standard BDF discretization
  else
  {
    this->sum_alphai_ui.equ(this->bdf.get_alpha(0) / this->get_time_step_size(), velocity[0]);
    for(unsigned int i = 1; i < velocity.size(); ++i)
    {
      this->sum_alphai_ui.add(this->bdf.get_alpha(i) / this->get_time_step_size(), velocity[i]);
    }
  }

  navier_stokes_operation->apply_mass_matrix_add(rhs_vec_momentum, this->sum_alphai_ui);

  /*
   *  Right-hand side viscous term:
   *  If a linear system of equations has to be solved,
   *  inhomogeneous parts of boundary face integrals of the viscous operator
   *  have to be shifted to the right-hand side of the equation.
   */
  if(this->param.equation_type == EquationType::Stokes ||
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit ||
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::ExplicitOIF)
  {
    navier_stokes_operation->rhs_add_viscous_term(rhs_vec_momentum, this->get_next_time());
  }
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::pressure_step()
{
  Timer timer;
  timer.restart();

  // compute right-hand side vector
  rhs_pressure();

  // extrapolate old solution to get a good initial estimate for the solver

  // calculate initial guess for pressure solve
  pressure_increment = 0.0;

  // extrapolate old solution to get a good initial estimate for the
  // pressure solution p_{n+1} at time t^{n+1}
  for(unsigned int i = 0; i < pressure.size(); ++i)
  {
    pressure_increment.add(this->extra.get_beta(i), pressure[i]);
  }

  // incremental formulation
  if(extra_pressure_gradient.get_order() > 0)
  {
    // Subtract extrapolation of pressure since the PPE is solved for the
    // pressure increment phi = p_{n+1} - sum_i (beta_pressure_extra_i * pressure_i)
    // where p_{n+1} is approximated by extrapolation of order J (=order of BDF scheme).
    // Note that divergence correction term in case of rotational formulation is not
    // considered when calculating a good initial guess for the solution of the PPE,
    // which will slightly increase the number of iterations compared to the standard
    // formulation of the pressure-correction scheme.
    for(unsigned int i = 0; i < extra_pressure_gradient.get_order(); ++i)
    {
      pressure_increment.add(-extra_pressure_gradient.get_beta(i), pressure[i]);
    }
  }

  // solve linear system of equations
  unsigned int iterations_pressure =
    navier_stokes_operation->solve_pressure(pressure_increment, rhs_vec_pressure);

  // calculate pressure p^{n+1} from pressure increment
  pressure_update();

  // Special case: pure Dirichlet BC's
  // Adjust the pressure level in order to allow a calculation of the pressure error.
  // This is necessary because otherwise the pressure solution moves away from the exact solution.
  // For some test cases it was found that ApplyZeroMeanValue works better than
  // ApplyAnalyticalSolutionInPoint
  if(this->param.pure_dirichlet_bc)
  {
    if(this->param.adjust_pressure_level == AdjustPressureLevel::ApplyAnalyticalSolutionInPoint)
    {
      navier_stokes_operation->shift_pressure(pressure_np, this->get_next_time());
    }
    else if(this->param.adjust_pressure_level == AdjustPressureLevel::ApplyZeroMeanValue)
    {
      set_zero_mean_value(pressure_np);
    }
    else if(this->param.adjust_pressure_level == AdjustPressureLevel::ApplyAnalyticalMeanValue)
    {
      navier_stokes_operation->shift_pressure_mean_value(pressure_np, this->get_next_time());
    }
    else
    {
      AssertThrow(false,
                  ExcMessage("Specified method to adjust pressure level is not implemented."));
    }
  }

  // write output
  if(this->get_time_step_number() % this->param.output_solver_info_every_timesteps == 0)
  {
    this->pcout << std::endl
                << "Solve Poisson equation for pressure p:" << std::endl
                << "  Iterations:        " << std::setw(6) << std::right << iterations_pressure
                << "\t Wall time [s]: " << std::scientific << timer.wall_time() << std::endl;
  }

  computing_times[1] += timer.wall_time();
  iterations[1] += iterations_pressure;
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::calculate_chi(double & chi) const
{
  if(this->param.formulation_viscous_term == FormulationViscousTerm::LaplaceFormulation)
  {
    chi = 1.0;
  }
  else if(this->param.formulation_viscous_term == FormulationViscousTerm::DivergenceFormulation)
  {
    chi = 2.0;
  }
  else
  {
    AssertThrow(this->param.formulation_viscous_term ==
                    FormulationViscousTerm::LaplaceFormulation &&
                  this->param.formulation_viscous_term ==
                    FormulationViscousTerm::DivergenceFormulation,
                ExcMessage("Not implemented!"));
  }
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::rhs_pressure()
{
  /*
   *  I. calculate divergence term
   */
  navier_stokes_operation->evaluate_velocity_divergence_term(rhs_vec_pressure_temp,
                                                             velocity_np,
                                                             this->get_next_time());

  rhs_vec_pressure.equ(-this->bdf.get_gamma0() / this->get_time_step_size(), rhs_vec_pressure_temp);


  /*
   *  II. calculate terms originating from inhomogeneous parts of boundary face integrals,
   *  i.e., pressure Dirichlet boundary conditions on Gamma_N and
   *  pressure Neumann boundary conditions on Gamma_D (always h=0 for pressure-correction scheme!)
   */
  navier_stokes_operation->rhs_ppe_laplace_add(rhs_vec_pressure, this->get_next_time());

  // incremental formulation of pressure-correction scheme
  for(unsigned int i = 0; i < extra_pressure_gradient.get_order(); ++i)
  {
    // set rhs_vec_pressure_temp to zero since rhs_ppe_laplace_add() adds into dst-vector
    rhs_vec_pressure_temp = 0.0;
    double const t        = this->get_previous_time(i);
    navier_stokes_operation->rhs_ppe_laplace_add(rhs_vec_pressure_temp, t);
    rhs_vec_pressure.add(-extra_pressure_gradient.get_beta(i), rhs_vec_pressure_temp);
  }

  // special case: pure Dirichlet BC's
  // Unclear if this is really necessary, because from a theoretical
  // point of view one would expect that the mean value of the rhs of the
  // presssure Poisson equation is zero if consistent Dirichlet boundary
  // conditions are prescribed.
  // In principle, it works (since the linear system of equations is consistent)
  // but we detected no convergence for some test cases and specific parameters.
  // Hence, for reasons of robustness we also solve a transformed linear system of equations
  // in case of the pressure-correction scheme.

  if(this->param.pure_dirichlet_bc)
    set_zero_mean_value(rhs_vec_pressure);
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::pressure_update()
{
  // First set pressure solution to zero.
  pressure_np = 0.0;

  // Rotational formulation only (this step is performed first in order
  // to avoid the storage of another temporary variable).
  if(this->param.rotational_formulation == true)
  {
    // Automatically sets pressure_np to zero before operator evaluation.
    navier_stokes_operation->evaluate_velocity_divergence_term(pressure_np,
                                                               velocity_np,
                                                               this->get_next_time());

    navier_stokes_operation->apply_inverse_pressure_mass_matrix(pressure_np, pressure_np);

    double chi = 0.0;
    calculate_chi(chi);

    pressure_np *= -chi * this->param.viscosity;
  }

  // This is done for both the incremental and the non-incremental formulation,
  // the standard and the rotational formulation.
  pressure_np.add(1.0, pressure_increment);

  // Incremental formulation only.

  // add extrapolation of pressure to the pressure-increment solution in order to obtain
  // the pressure solution at the end of the time step, i.e.,
  // p^{n+1} = (pressure_increment)^{n+1} + sum_i (beta_pressure_extrapolation_i * p^{n-i});
  for(unsigned int i = 0; i < extra_pressure_gradient.get_order(); ++i)
  {
    pressure_np.add(extra_pressure_gradient.get_beta(i), pressure[i]);
  }
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::rhs_projection()
{
  /*
   *  I. calculate mass matrix term
   */
  navier_stokes_operation->apply_mass_matrix(rhs_vec_projection, velocity_np);

  /*
   *  II. calculate pressure gradient term including boundary condition g_p(t_{n+1})
   */
  navier_stokes_operation->evaluate_pressure_gradient_term(rhs_vec_projection_temp,
                                                           pressure_increment,
                                                           this->get_next_time());

  rhs_vec_projection.add(-this->get_time_step_size() / this->bdf.get_gamma0(),
                         rhs_vec_projection_temp);

  /*
   *  III. pressure gradient term: boundary conditions g_p(t_{n-i})
   *       in case of incremental formulation of pressure-correction scheme
   */
  for(unsigned int i = 0; i < extra_pressure_gradient.get_order(); ++i)
  {
    // evaluate inhomogeneous parts of boundary face integrals
    double const current_time = this->get_previous_time(i);
    navier_stokes_operation->rhs_pressure_gradient_term(rhs_vec_projection_temp, current_time);

    rhs_vec_projection.add(-extra_pressure_gradient.get_beta(i) * this->get_time_step_size() /
                             this->bdf.get_gamma0(),
                           rhs_vec_projection_temp);
  }
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::projection_step()
{
  Timer timer;
  timer.restart();

  // compute right-hand-side vector
  rhs_projection();

  VectorType velocity_extrapolated;

  unsigned int iterations_projection = 0;

  // extrapolate velocity to time t_n+1 and use this velocity field to
  // calculate the penalty parameter for the divergence and continuity penalty term
  if(this->param.use_divergence_penalty == true || this->param.use_continuity_penalty == true)
  {
    velocity_extrapolated.reinit(velocity[0]);
    for(unsigned int i = 0; i < velocity.size(); ++i)
      velocity_extrapolated.add(this->extra.get_beta(i), velocity[i]);

    navier_stokes_operation->update_projection_operator(velocity_extrapolated,
                                                        this->get_time_step_size());

    // solve linear system of equations
    iterations_projection =
      navier_stokes_operation->solve_projection(velocity_np, rhs_vec_projection);
  }
  else // no penalty terms, simply apply inverse mass matrix
  {
    navier_stokes_operation->apply_inverse_mass_matrix(velocity_np, rhs_vec_projection);
  }

  // write output
  if(this->get_time_step_number() % this->param.output_solver_info_every_timesteps == 0)
  {
    this->pcout << std::endl
                << "Solve projection step for intermediate velocity:" << std::endl
                << "  Iterations:        " << std::setw(6) << std::right << iterations_projection
                << "\t Wall time [s]: " << std::scientific << timer.wall_time() << std::endl;
  }

  computing_times[2] += timer.wall_time();
  iterations[2] += iterations_projection;
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::
  prepare_vectors_for_next_timestep()
{
  push_back(velocity);
  velocity[0].swap(velocity_np);

  push_back(pressure);
  pressure[0].swap(pressure_np);

  if(this->param.equation_type == EquationType::NavierStokes &&
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit)
  {
    push_back(vec_convective_term);
  }

  if(extra_pressure_gradient.get_order() > 0)
  {
    push_back(vec_pressure_gradient_term);
  }
}

template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::solve_steady_problem()
{
  this->pcout << std::endl << "Starting time loop ..." << std::endl;

  // pseudo-time integration in order to solve steady-state problem
  bool converged = false;

  if(this->param.convergence_criterion_steady_problem ==
     ConvergenceCriterionSteadyProblem::SolutionIncrement)
  {
    while(!converged && this->get_time_step_number() <= this->param.max_number_of_time_steps)
    {
      // save solution from previous time step
      velocity_tmp = velocity[0];
      pressure_tmp = pressure[0];

      // calculate normm of solution
      double const norm_u = velocity_tmp.l2_norm();
      double const norm_p = pressure_tmp.l2_norm();
      double const norm   = std::sqrt(norm_u * norm_u + norm_p * norm_p);

      // solve time step
      this->do_timestep();

      // calculate increment:
      // increment = solution_{n+1} - solution_{n}
      //           = solution[0] - solution_tmp
      velocity_tmp *= -1.0;
      pressure_tmp *= -1.0;
      velocity_tmp.add(1.0, velocity[0]);
      pressure_tmp.add(1.0, pressure[0]);

      double const incr_u   = velocity_tmp.l2_norm();
      double const incr_p   = pressure_tmp.l2_norm();
      double const incr     = std::sqrt(incr_u * incr_u + incr_p * incr_p);
      double       incr_rel = 1.0;
      if(norm > 1.0e-10)
        incr_rel = incr / norm;

      // write output
      if(this->get_time_step_number() % this->param.output_solver_info_every_timesteps == 0)
      {
        this->pcout << std::endl
                    << "Norm of solution increment:" << std::endl
                    << "  ||incr_abs|| = " << std::scientific << std::setprecision(10) << incr
                    << std::endl
                    << "  ||incr_rel|| = " << std::scientific << std::setprecision(10) << incr_rel
                    << std::endl;
      }

      // check convergence
      if(incr < this->param.abs_tol_steady || incr_rel < this->param.rel_tol_steady)
      {
        converged = true;
      }
    }
  }
  else if(this->param.convergence_criterion_steady_problem ==
          ConvergenceCriterionSteadyProblem::ResidualSteadyNavierStokes)
  {
    double const initial_residual = evaluate_residual();

    while(!converged && this->get_time_step_number() <= this->param.max_number_of_time_steps)
    {
      this->do_timestep();

      // check convergence by evaluating the residual of
      // the steady-state incompressible Navier-Stokes equations
      double const residual = evaluate_residual();

      if(residual < this->param.abs_tol_steady ||
         residual / initial_residual < this->param.rel_tol_steady)
      {
        converged = true;
      }
    }
  }
  else
  {
    AssertThrow(false, ExcMessage("not implemented."));
  }

  AssertThrow(
    converged == true,
    ExcMessage(
      "Maximum number of time steps exceeded! This might be due to the fact that "
      "(i) the maximum number of iterations is simply too small to reach a steady solution, "
      "(ii) the problem is unsteady so that the applied solution approach is inappropriate, "
      "(iii) some of the solver tolerances are in conflict."));

  this->pcout << std::endl << "... done!" << std::endl;
}

template<int dim, typename Number, typename NavierStokesOperation>
double
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::evaluate_residual()
{
  navier_stokes_operation->evaluate_nonlinear_residual_steady(
    velocity_np, pressure_np, velocity[0], pressure[0], this->get_time());

  double const norm_u = velocity_np.l2_norm();
  double const norm_p = pressure_np.l2_norm();

  double residual = std::sqrt(norm_u * norm_u + norm_p * norm_p);

  // write output
  if((this->get_time_step_number() - 1) % this->param.output_solver_info_every_timesteps == 0)
  {
    this->pcout << std::endl
                << "Norm of residual of steady Navier-Stokes equations:" << std::endl
                << "  ||r|| = " << std::scientific << std::setprecision(10) << residual
                << std::endl;
  }

  return residual;
}


template<int dim, typename Number, typename NavierStokesOperation>
void
TimeIntBDFPressureCorrection<dim, Number, NavierStokesOperation>::analyze_computing_times() const
{
  std::string  names[3]     = {"Momentum     ", "Pressure     ", "Projection   "};
  unsigned int N_time_steps = this->get_time_step_number() - 1;

  // iterations
  this->pcout << std::endl
              << "_________________________________________________________________________________"
              << std::endl
              << std::endl
              << "Average number of iterations:" << std::endl;

  for(unsigned int i = 0; i < iterations.size(); ++i)
  {
    this->pcout << "  Step " << i + 1 << ": " << names[i];

    if(i == 0) // momentum
    {
      if(this->param.equation_type == EquationType::Stokes ||
         this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Explicit ||
         this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::ExplicitOIF)
      {
        this->pcout << std::scientific << std::setprecision(4) << std::setw(10)
                    << iterations[i] / (double)N_time_steps << " linear iterations" << std::endl;
      }
      else
      {
        double n_iter_nonlinear          = (double)N_iter_nonlinear_momentum / (double)N_time_steps;
        double n_iter_linear_accumulated = (double)iterations[0] / (double)N_time_steps;

        this->pcout << std::scientific << std::setprecision(4) << std::setw(10) << n_iter_nonlinear
                    << " nonlinear iterations" << std::endl;

        this->pcout << "                       " << std::scientific << std::setprecision(4)
                    << std::setw(10) << n_iter_linear_accumulated
                    << " linear iterations (accumulated)" << std::endl;

        this->pcout << "                       " << std::scientific << std::setprecision(4)
                    << std::setw(10) << n_iter_linear_accumulated / n_iter_nonlinear
                    << " linear iterations (per nonlinear iteration)" << std::endl;
      }
    }
    else
    {
      this->pcout << std::scientific << std::setprecision(4) << std::setw(10)
                  << iterations[i] / (double)N_time_steps << std::endl;
    }
  }
  this->pcout << "_________________________________________________________________________________"
              << std::endl
              << std::endl;

  // Computing times
  this->pcout << std::endl
              << "_________________________________________________________________________________"
              << std::endl
              << std::endl
              << "Computing times:          min        avg        max        rel      p_min  p_max "
              << std::endl;

  double total_avg_time = 0.0;

  for(unsigned int i = 0; i < computing_times.size(); ++i)
  {
    Utilities::MPI::MinMaxAvg data =
      Utilities::MPI::min_max_avg(computing_times[i], MPI_COMM_WORLD);
    total_avg_time += data.avg;
  }

  for(unsigned int i = 0; i < computing_times.size(); ++i)
  {
    Utilities::MPI::MinMaxAvg data =
      Utilities::MPI::min_max_avg(computing_times[i], MPI_COMM_WORLD);
    this->pcout << "  Step " << i + 1 << ": " << names[i] << std::scientific << std::setprecision(4)
                << std::setw(10) << data.min << " " << std::setprecision(4) << std::setw(10)
                << data.avg << " " << std::setprecision(4) << std::setw(10) << data.max << " "
                << std::setprecision(4) << std::setw(10) << data.avg / total_avg_time << "  "
                << std::setw(6) << std::left << data.min_index << " " << std::setw(6) << std::left
                << data.max_index << std::endl;
  }

  this->pcout << "  Time in steps 1-" << computing_times.size() << ":              "
              << std::setprecision(4) << std::setw(10) << total_avg_time << "            "
              << std::setprecision(4) << std::setw(10) << total_avg_time / total_avg_time
              << std::endl;

  this->pcout << std::endl
              << "Number of time steps =            " << std::left << N_time_steps << std::endl
              << "Average wall time per time step = " << std::scientific << std::setprecision(4)
              << total_avg_time / (double)N_time_steps << std::endl
              << std::endl;

  // overall wall time including postprocessing
  Utilities::MPI::MinMaxAvg data = Utilities::MPI::min_max_avg(this->total_time, MPI_COMM_WORLD);
  this->pcout << "Total wall time in [s] =          " << std::scientific << std::setprecision(4)
              << data.avg << std::endl;

  unsigned int N_mpi_processes = Utilities::MPI::n_mpi_processes(MPI_COMM_WORLD);

  this->pcout << "Number of MPI processes =         " << N_mpi_processes << std::endl
              << "Computational costs in [CPUs] =   " << data.avg * (double)N_mpi_processes
              << std::endl
              << "Computational costs in [CPUh] =   " << data.avg * (double)N_mpi_processes / 3600.0
              << std::endl
              << "_________________________________________________________________________________"
              << std::endl
              << std::endl;
}


} // namespace IncNS


#endif /* INCLUDE_INCOMPRESSIBLE_NAVIER_STOKES_TIME_INTEGRATION_TIME_INT_BDF_PRESSURE_CORRECTION_H_ \
        */
