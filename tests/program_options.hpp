#ifndef __TESTS_PROGRAM_OPTIONS__
#define __TESTS_PROGRAM_OPTIONS__
/* To be included by all the tests */

#include <crab/common/debug.hpp>
#include <boost/program_options.hpp>

#include <iostream>

namespace {
  bool stats_enabled = false;                                                                         
  #define SET_TEST_OPTIONS(ARGC,ARGV)                                                                 \
  boost::program_options::options_description po("Test Options");                                     \
  po.add_options()                                                                                    \
  ("help", "Print help message");                                                                     \
  po.add_options()                                                                                    \
  ("log",  boost::program_options::value<std::vector<std::string> >(), "Enable specified log level"); \
  po.add_options()                                                                                    \
  ("verbose",  boost::program_options::value<unsigned>(), "Enable verbosity level");                  \
  po.add_options()                                                                                    \
  ("stats",boost::program_options::bool_switch(&stats_enabled), "Enable stats");                      \
  po.add_options()                                                                                    \
  ("disable-warnings", "Disable warning messages");                                                   \
  po.add_options()                                                                                    \
  ("sanity", "Enable sanity checks");                                                                 \
  boost::program_options::options_description cmmdline_options;                                       \
  cmmdline_options.add(po);                                                                           \
  boost::program_options::variables_map vm;                                                           \
  boost::program_options::positional_options_description p;                                           \
  boost::program_options::store(boost::program_options::command_line_parser(ARGC, ARGV).              \
            options(cmmdline_options).                                                                \
            positional(p).                                                                            \
              run(), vm);                                                                             \
  boost::program_options::notify(vm);                                                                 \
  if (vm.count("help")) {                                                                             \
     std::cout << po << "\n";                                                                         \
     return 0;                                                                                        \
  }                                                                                                   \
  if (vm.count("log")) {                                                                              \
    std::vector<std::string> loggers = vm ["log"].as<std::vector<std::string> > ();                   \
    for(unsigned int i=0; i<loggers.size (); i++)                                                     \
      crab::CrabEnableLog (loggers [i]);                                                              \
  }                                                                                                   \
  if (vm.count("verbose")) {						                              \
      crab::CrabEnableVerbosity(vm["verbose"].as<unsigned>());                                        \
  }									                              \
  if (vm.count("disable-warnings")) {						                      \
    crab::CrabEnableWarningMsg(false);					                              \
  }									                              \
  if (vm.count("sanity")) {						                              \
    crab::CrabEnableSanityChecks(true);					                              \
  }	  
  
} //end namespace
#endif
