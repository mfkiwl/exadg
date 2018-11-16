/*
 * time_step_calculation.h
 *
 *  Created on: May 30, 2016
 *      Author: fehn
 */

#ifndef INCLUDE_TIME_INTEGRATION_TIME_STEP_CALCULATION_H_
#define INCLUDE_TIME_INTEGRATION_TIME_STEP_CALCULATION_H_

#define CFL_BASED_ON_MINIMUM_COMPONENT

#include <deal.II/base/function.h>
#include <deal.II/lac/la_parallel_vector.h>
#include <deal.II/matrix_free/fe_evaluation.h>
#include "../functionalities/calculate_characteristic_element_length.h"

using namespace dealii;

/*
 *  limit the maximum increase/decrease of the time step size
 */
inline void
limit_time_step_change(double & new_time_step, double const & last_time_step, double const & fac)
{
  if(new_time_step >= fac * last_time_step)
  {
    new_time_step = fac * last_time_step;
  }
  else if(new_time_step <= last_time_step / fac)
  {
    new_time_step = last_time_step / fac;
  }
}

/*
 * Decrease time_step in order to exactly hit end_time.
 */
inline double
adjust_time_step_to_hit_end_time(double const start_time,
                                 double const end_time,
                                 double const time_step)
{
  return (end_time - start_time) / (1 + int((end_time - start_time) / time_step));
}

/*
 * This function calculates the time step size for a given time step size and a specified number of
 * refinements, where the time step size is reduced by a factor of 2 for each refinement level.
 */
inline double
calculate_const_time_step(double const dt, unsigned int const n_refine_time)
{
  double const time_step = dt / std::pow(2., n_refine_time);

  return time_step;
}

/*
 * This function calculates the time step such that the spatial and temporal errors are of the same
 * order of magnitude:
 *
 *    spatial error: e = C_h * h^{p+1}
 *    temporal error: e = C_dt * dt^{n} (n: order of time integration method)
 */
inline double
calculate_time_step_max_efficiency(double const       c_eff,
                                   double const       global_min_cell_diameter,
                                   unsigned int const fe_degree,
                                   unsigned int const order_time_integration,
                                   unsigned int const n_refine_time)
{
  double const exponent = (double)(fe_degree + 1) / order_time_integration;
  double const time_step =
    c_eff * std::pow(global_min_cell_diameter, exponent) / std::pow(2., n_refine_time);

  return time_step;
}

/*
 * This function calculates the maximum velocity for a given velocity field (which is known
 * analytically). The maximum value is defined as the maximum velocity at the cell center.
 */
template<int dim>
inline double
calculate_max_velocity(Triangulation<dim> const &     triangulation,
                       std::shared_ptr<Function<dim>> velocity,
                       double const                   time)
{
  typename Triangulation<dim>::active_cell_iterator cell = triangulation.begin_active(),
                                                    endc = triangulation.end();

  double U = 0.0, max_U = std::numeric_limits<double>::min();

  Tensor<1, dim, double> vel;
  velocity->set_time(time);

  for(; cell != endc; ++cell)
  {
    if(cell->is_locally_owned())
    {
      // calculate maximum velocity
      Point<dim> point = cell->center();

      for(unsigned int d = 0; d < dim; ++d)
        vel[d] = velocity->value(point, d);

      U = vel.norm();
      if(U > max_U)
        max_U = U;
    }
  }
  double const global_max_U = Utilities::MPI::max(max_U, MPI_COMM_WORLD);

  return global_max_U;
}

/*
 * This function calculates the time step size according to the CFL condition where a global
 * criterion is used based on the maximum velocity and the minimum mesh size. Hence, this leads to a
 * conservative estimate of the time step size.
 *
 *    cfl/k^{exponent_fe_degree} = || U || * time_step / h
 */
inline double
calculate_time_step_cfl_global(double const       cfl,
                               double const       max_velocity,
                               double const       global_min_cell_diameter,
                               unsigned int const fe_degree,
                               double const       exponent_fe_degree = 2.0)
{
  double const time_step =
    cfl / pow(fe_degree, exponent_fe_degree) * global_min_cell_diameter / max_velocity;

  return time_step;
}

/*
 * This function calculates the time step according to the formula
 *
 *    diffusion_number/k^{exponent_fe_degree} = diffusivity * time_step / h²
 */
inline double
calculate_const_time_step_diff(double const       diffusion_number,
                               double const       diffusivity,
                               double const       global_min_cell_diameter,
                               unsigned int const fe_degree,
                               double const       exponent_fe_degree = 3.0)
{
  double const time_step = diffusion_number / pow(fe_degree, exponent_fe_degree) *
                           pow(global_min_cell_diameter, 2.0) / diffusivity;

  return time_step;
}

/*
 * Calculate time step size according to local CFL criterion where the velocity field is a
 * prescribed analytical function.
 */
template<int dim, int fe_degree, typename value_type>
inline double
calculate_time_step_cfl_local(MatrixFree<dim, value_type> const &  data,
                              unsigned int const                   dof_index,
                              unsigned int const                   quad_index,
                              std::shared_ptr<Function<dim>> const velocity,
                              double const                         time,
                              double const                         cfl,
                              double const                         exponent_fe_degree)
{
  FEEvaluation<dim, fe_degree, fe_degree + 1, dim, value_type> fe_eval(data, dof_index, quad_index);

  double new_time_step = std::numeric_limits<double>::max();

  double const cfl_p = cfl / pow(fe_degree, exponent_fe_degree);

  // loop over cells of processor
  for(unsigned int cell = 0; cell < data.n_macro_cells(); ++cell)
  {
    VectorizedArray<value_type> delta_t_cell =
      make_vectorized_array<value_type>(std::numeric_limits<value_type>::max());

    fe_eval.reinit(cell);

    // loop over quadrature points
    for(unsigned int q = 0; q < fe_eval.n_q_points; ++q)
    {
      Point<dim, VectorizedArray<value_type>> q_point = fe_eval.quadrature_point(q);

      Tensor<1, dim, VectorizedArray<value_type>> u_x =
        evaluate_vectorial_function(velocity, q_point, time);
      Tensor<2, dim, VectorizedArray<value_type>> invJ  = fe_eval.inverse_jacobian(q);
      invJ                                              = transpose(invJ);
      Tensor<1, dim, VectorizedArray<value_type>> ut_xi = invJ * u_x;

#ifdef CFL_BASED_ON_MINIMUM_COMPONENT
      for(unsigned int d = 0; d < dim; ++d)
        delta_t_cell = std::min(delta_t_cell, cfl_p / (std::abs(ut_xi[d])));
#else
      delta_t_cell = std::min(delta_t_cell, cfl_p / ut_xi.norm());
#endif
    }

    // loop over vectorized array
    double dt = std::numeric_limits<double>::max();
    for(unsigned int v = 0; v < VectorizedArray<value_type>::n_array_elements; ++v)
    {
      dt = std::min(dt, delta_t_cell[v]);
    }

    new_time_step = std::min(new_time_step, dt);
  }

  // find minimum over all processors
  new_time_step = Utilities::MPI::min(new_time_step, MPI_COMM_WORLD);

  return new_time_step;
}

/*
 * Calculate time step size according to local CFL criterion where the velocity field is a numerical
 * solution field.
 */
template<int dim, int fe_degree, typename value_type>
inline double
calculate_time_step_cfl_local(MatrixFree<dim, value_type> const &                    data,
                              unsigned int const                                     dof_index,
                              unsigned int const                                     quad_index,
                              LinearAlgebra::distributed::Vector<value_type> const & velocity,
                              double const                                           cfl,
                              double const exponent_fe_degree)
{
  FEEvaluation<dim, fe_degree, fe_degree + 1, dim, value_type> fe_eval(data, dof_index, quad_index);

  double new_time_step = std::numeric_limits<double>::max();

  double const cfl_p = cfl / pow(fe_degree, exponent_fe_degree);

  // loop over cells of processor
  for(unsigned int cell = 0; cell < data.n_macro_cells(); ++cell)
  {
    VectorizedArray<value_type> delta_t_cell =
      make_vectorized_array<value_type>(std::numeric_limits<value_type>::max());

    Tensor<2, dim, VectorizedArray<value_type>> invJ;
    Tensor<1, dim, VectorizedArray<value_type>> u_x;
    Tensor<1, dim, VectorizedArray<value_type>> ut_xi;

    fe_eval.reinit(cell);
    fe_eval.read_dof_values(velocity);
    fe_eval.evaluate(true, false);

    // loop over quadrature points
    for(unsigned int q = 0; q < fe_eval.n_q_points; ++q)
    {
      u_x   = fe_eval.get_value(q);
      invJ  = fe_eval.inverse_jacobian(q);
      invJ  = transpose(invJ);
      ut_xi = invJ * u_x;

#ifdef CFL_BASED_ON_MINIMUM_COMPONENT
      for(unsigned int d = 0; d < dim; ++d)
        delta_t_cell = std::min(delta_t_cell, cfl_p / (std::abs(ut_xi[d])));
#else
      delta_t_cell = std::min(delta_t_cell, cfl_p / ut_xi.norm());
#endif
    }

    // loop over vectorized array
    double dt = std::numeric_limits<double>::max();
    for(unsigned int v = 0; v < VectorizedArray<value_type>::n_array_elements; ++v)
    {
      dt = std::min(dt, delta_t_cell[v]);
    }

    new_time_step = std::min(new_time_step, dt);
  }

  // find minimum over all processors
  new_time_step = Utilities::MPI::min(new_time_step, MPI_COMM_WORLD);

  return new_time_step;
}


// template<int dim, int fe_degree, typename value_type>
// double
// calculate_adaptive_time_step_diffusion(MatrixFree<dim, value_type> const & data,
//                                       unsigned int const                  dof_index,
//                                       unsigned int const                  quad_index,
//                                       LinearAlgebra::distributed::Vector<value_type> const & vt,
//                                       double const viscosity,
//                                       double const d,
//                                       double const last_time_step,
//                                       bool const   use_limiter        = true,
//                                       double const exponent_fe_degree = 3)
//{
//  FEEvaluation<dim, fe_degree, fe_degree + 1, 1, value_type> fe_eval(data, dof_index, quad_index);
//
//  double new_time_step = std::numeric_limits<value_type>::max();
//
//  double const d_p = d / pow(fe_degree, exponent_fe_degree);
//
//  // loop over cells of processor
//  for(unsigned int cell = 0; cell < data.n_macro_cells(); ++cell)
//  {
//    VectorizedArray<value_type> h;
//    for(unsigned int v = 0; v < VectorizedArray<value_type>::n_array_elements; v++)
//    {
//      typename DoFHandler<dim>::cell_iterator dcell = data.get_cell_iterator(cell, v);
//
//      h[v] = dcell->minimum_vertex_distance();
//    }
//    VectorizedArray<value_type> delta_t_cell =
//      make_vectorized_array<value_type>(std::numeric_limits<value_type>::max());
//    VectorizedArray<value_type> vt_val;
//
//    fe_eval.reinit(cell);
//    fe_eval.read_dof_values(vt);
//    fe_eval.evaluate(true, false);
//
//    // loop over quadrature points
//    for(unsigned int q = 0; q < fe_eval.n_q_points; ++q)
//    {
//      vt_val = fe_eval.get_value(q);
//
//      // TODO Benjamin: use local shortest length, not global one
//      // vt should be larger than zero
//      delta_t_cell = std::min(delta_t_cell, d_p * h * h / (std::abs((vt_val + viscosity) * 1.5)));
//    }
//
//    // loop over vectorized array
//    value_type dt = std::numeric_limits<value_type>::max();
//    for(unsigned int v = 0; v < VectorizedArray<value_type>::n_array_elements; ++v)
//    {
//      dt = std::min(dt, delta_t_cell[v]);
//    }
//
//    new_time_step = std::min(new_time_step, dt);
//  }
//
//  // find minimum over all processors
//  new_time_step = Utilities::MPI::min(new_time_step, MPI_COMM_WORLD);
//
//  // TODO
//  double const factor = 1.2;
//  limit_time_step_change(new_time_step, last_time_step, factor);
//
//  return new_time_step;
//}


#endif /* INCLUDE_TIME_INTEGRATION_TIME_STEP_CALCULATION_H_ */
