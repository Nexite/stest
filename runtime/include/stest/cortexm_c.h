/* stest Cortex-M core C API, called by the host CMSIS shim (chips/cmsis_host/
 * cmsis_compiler.h) for intrinsics that touch core state. Every function routes to
 * the Cortexm installed with Cortexm::install(); with none installed, or if the
 * model throws, it calls stest_rt_abort (exit 70). */
#ifndef STEST_CORTEXM_C_H
#define STEST_CORTEXM_C_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void stest_rt_wfi(void);                 /* __WFI / __WFE */
void stest_rt_enable_irq(void);          /* PRIMASK = 0 (delivery point) */
void stest_rt_disable_irq(void);         /* PRIMASK = 1 */
uint32_t stest_rt_get_primask(void);
void stest_rt_set_primask(uint32_t v);   /* bit 0 */
uint32_t stest_rt_get_basepri(void);
void stest_rt_set_basepri(uint32_t v);
void stest_rt_set_basepri_max(uint32_t v);
uint32_t stest_rt_get_faultmask(void);
void stest_rt_set_faultmask(uint32_t v); /* bit 0 */
uint32_t stest_rt_get_ipsr(void);        /* active exception number, 0 in thread mode */

#ifdef __cplusplus
}
#endif
#endif
