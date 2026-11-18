/**
 * @file rkdiag.h
 * @brief One switch for the periodic statistic lines, counters and bug-hunt checks of the Vulkan work.
 *
 * <FS> Rekindled 2026-11-16: a default run logged about 86 lines a second (1.3 MB a minute) of statistics written for the work of the last weeks, and hashed every block compressed texture for a bug hunt that is over.
 * They are off unless LL_VK_DIAG is set in the environment (the lap harness sets it, the launchers do not). Features the viewer needs do not use this: only reports and checks.
 */
#ifndef LL_LLDIAG_H
#define LL_LLDIAG_H

#include <cstdlib>

inline bool rk_diag_on()
{
    static const bool on = std::getenv("LL_VK_DIAG") != nullptr;
    return on;
}

// LL_DIAG("Tag") << ... << LL_ENDL;  is LL_INFOS("Tag") when LL_VK_DIAG is set; otherwise nothing is formatted and nothing is evaluated (a hash in the stream expression is not computed)
#define LL_DIAG(...) if (!rk_diag_on()) {} else LL_INFOS(__VA_ARGS__)

#endif // LL_LLDIAG_H
