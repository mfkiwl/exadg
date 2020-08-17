/*
 * poisson.cc
 *
 *  Created on: May, 2019
 *      Author: fehn
 */

// driver
#include "../include/poisson/driver.h"

// infrastructure for parameter studies
#include "../include/utilities/parameter_study.h"

// applications
#include "poisson_test_cases/gaussian/gaussian.h"
#include "poisson_test_cases/lung/lung.h"
#include "poisson_test_cases/lung_tubus/lung_tubus.h"
#include "poisson_test_cases/nozzle/nozzle.h"
#include "poisson_test_cases/sine/sine.h"
#include "poisson_test_cases/slit/slit.h"
#include "poisson_test_cases/template/template.h"
#include "poisson_test_cases/torus/torus.h"

namespace ExaDG
{
class ApplicationSelector
{
public:
  template<int dim, typename Number>
  std::shared_ptr<Poisson::ApplicationBase<dim, Number>>
  get_application(std::string input_file)
  {
    parse_name_parameter(input_file);

    std::shared_ptr<Poisson::ApplicationBase<dim, Number>> app;
    if(name == "Template")
      app.reset(new Poisson::Template::Application<dim, Number>(input_file));
    else if(name == "Gaussian")
      app.reset(new Poisson::Gaussian::Application<dim, Number>(input_file));
    else if(name == "Sine")
      app.reset(new Poisson::Sine::Application<dim, Number>(input_file));
    else if(name == "Slit")
      app.reset(new Poisson::Slit::Application<dim, Number>(input_file));
    else if(name == "Torus")
      app.reset(new Poisson::Torus::Application<dim, Number>(input_file));
    else if(name == "Nozzle")
      app.reset(new Poisson::Nozzle::Application<dim, Number>(input_file));
    else if(name == "LungTubus")
      app.reset(new Poisson::LungTubus::Application<dim, Number>(input_file));
    else if(name == "Lung")
      app.reset(new Poisson::Lung::Application<dim, Number>(input_file));
    else
      AssertThrow(false, ExcMessage("This application does not exist!"));

    return app;
  }

  template<int dim, typename Number>
  void
  add_parameters(dealii::ParameterHandler & prm, std::string const & input_file)
  {
    // if application is known, also add application-specific parameters
    try
    {
      std::shared_ptr<Poisson::ApplicationBase<dim, Number>> app =
        get_application<dim, Number>(input_file);

      add_name_parameter(prm);
      app->add_parameters(prm);
    }
    catch(...) // if application is unknown, only add name of application to parameters
    {
      add_name_parameter(prm);
    }
  }

private:
  void
  add_name_parameter(ParameterHandler & prm)
  {
    prm.enter_subsection("Application");
    prm.add_parameter("Name", name, "Name of application.");
    prm.leave_subsection();
  }

  void
  parse_name_parameter(std::string input_file)
  {
    dealii::ParameterHandler prm;
    add_name_parameter(prm);
    prm.parse_input(input_file, "", true, true);
  }

  std::string name = "MyApp";
};

void
create_input_file(std::string const & input_file)
{
  dealii::ParameterHandler prm;

  ParameterStudy parameter_study;
  parameter_study.add_parameters(prm);

  // we have to assume a default dimension and default Number type
  // for the automatic generation of a default input file
  unsigned int const Dim = 2;
  typedef double     Number;

  ApplicationSelector selector;
  selector.add_parameters<Dim, Number>(prm, input_file);

  prm.print_parameters(input_file,
                       dealii::ParameterHandler::Short |
                         dealii::ParameterHandler::KeepDeclarationOrder);
}

template<int dim, typename Number>
void
run(std::vector<Timings> & timings,
    std::string const &    input_file,
    unsigned int const     degree,
    unsigned int const     refine_space,
    unsigned int const     n_cells_1d,
    MPI_Comm const &       mpi_comm)
{
  Timer timer;
  timer.restart();

  std::shared_ptr<Poisson::Driver<dim, Number>> driver;
  driver.reset(new Poisson::Driver<dim, Number>(mpi_comm));

  ApplicationSelector selector;

  std::shared_ptr<Poisson::ApplicationBase<dim, Number>> application =
    selector.get_application<dim, Number>(input_file);

  application->set_subdivisions_hypercube(n_cells_1d);
  driver->setup(application, degree, refine_space);

  driver->solve();

  Timings timing = driver->print_statistics(timer.wall_time());
  timings.push_back(timing);
}
} // namespace ExaDG

int
main(int argc, char ** argv)
{
  dealii::Utilities::MPI::MPI_InitFinalize mpi(argc, argv, 1);

  MPI_Comm mpi_comm(MPI_COMM_WORLD);

  std::string input_file;

  if(argc == 1)
  {
    if(dealii::Utilities::MPI::this_mpi_process(mpi_comm) == 0)
    {
      std::cout << "To run the program, use:      ./poisson input_file" << std::endl
                << "To setup the input file, use: ./poisson input_file --help" << std::endl;
    }

    return 0;
  }
  else if(argc >= 2)
  {
    input_file = std::string(argv[1]);

    if(argc == 3 && std::string(argv[2]) == "--help")
    {
      if(dealii::Utilities::MPI::this_mpi_process(mpi_comm) == 0)
        ExaDG::create_input_file(input_file);

      return 0;
    }
  }

  ExaDG::ParameterStudy study(input_file);

  // fill resolution vector depending on the operator_type
  study.fill_resolution_vector(&ExaDG::Poisson::get_dofs_per_element,
                               enum_to_string(ExaDG::Poisson::OperatorType::MatrixFree));

  std::vector<ExaDG::Timings> timings;

  // loop over resolutions vector and run simulations
  for(auto iter = study.resolutions.begin(); iter != study.resolutions.end(); ++iter)
  {
    unsigned int const degree       = std::get<0>(*iter);
    unsigned int const refine_space = std::get<1>(*iter);
    unsigned int const n_cells_1d   = std::get<2>(*iter);

    if(study.dim == 2 && study.precision == "float")
      ExaDG::run<2, float>(timings, input_file, degree, refine_space, n_cells_1d, mpi_comm);
    else if(study.dim == 2 && study.precision == "double")
      ExaDG::run<2, double>(timings, input_file, degree, refine_space, n_cells_1d, mpi_comm);
    else if(study.dim == 3 && study.precision == "float")
      ExaDG::run<3, float>(timings, input_file, degree, refine_space, n_cells_1d, mpi_comm);
    else if(study.dim == 3 && study.precision == "double")
      ExaDG::run<3, double>(timings, input_file, degree, refine_space, n_cells_1d, mpi_comm);
    else
      AssertThrow(false,
                  dealii::ExcMessage("Only dim = 2|3 and precision=float|double implemented."));
  }

  print_results(timings, mpi_comm);

  return 0;
}
