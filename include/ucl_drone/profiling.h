/*!
 *  \file profiling.h
 *  \brief Common definition for PROFILING MACRO
 *  \author Arnaud Jacques & Alexandre Leclere
 *  \date 2016
 */

#ifndef UCL_PROFILING_H
#define UCL_PROFILING_H
#define USE_PROFILING

#ifdef USE_PROFILING

//! begin counter
#define TIC(name) double profiling_##name = ros::Time::now().toSec();

//! stop counter and display message in the standard output
#define TOC_DISPLAY(name, msg)                            \
  std::cout << "\033[1;36m[TIC TOC]: " << msg << ": "     \
            << ros::Time::now().toSec() - profiling_##name << "\033[0m\n";
#define TOC(name) ros::Time::now().toSec() - profiling_##name;

#else

#define TIC(name)  //!< begin counter
#define TOC(name)  //!< end counter and display message in the standard output

#endif /* USE_PROFILING */

#endif /* UCL_PROFILING_H */
